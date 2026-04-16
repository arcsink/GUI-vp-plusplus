#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

enum test_mode {
    MODE_NO_FENCE = 0,
    MODE_FENCE_RW_RW = 1,
};

struct shared_state {
    pthread_barrier_t iter_barrier;
    volatile uint64_t x __attribute__((aligned(64)));
    volatile uint64_t y __attribute__((aligned(64)));
    volatile uint64_t r0 __attribute__((aligned(64)));
    volatile uint64_t r1 __attribute__((aligned(64)));
    long iterations;
    int mode;
};

struct thread_ctx {
    struct shared_state *shared;
    int cpu;
    int is_thread0;
};

static inline void compiler_barrier(void) {
    __asm__ volatile("" ::: "memory");
}

static inline void store_u64(volatile uint64_t *ptr, uint64_t value) {
    __asm__ volatile("sd %1, 0(%0)" :: "r"(ptr), "r"(value) : "memory");
}

static inline uint64_t load_u64(volatile uint64_t *ptr) {
    uint64_t value;
    __asm__ volatile("ld %0, 0(%1)" : "=r"(value) : "r"(ptr) : "memory");
    return value;
}

static inline void do_fence_rw_rw(void) {
    __asm__ volatile("fence rw,rw" ::: "memory");
}

static void try_pin_to_cpu(int cpu) {
    cpu_set_t mask;

    if (cpu < 0) {
        return;
    }

    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
        fprintf(stderr, "[guivp-fence-sb] warn: sched_setaffinity(cpu=%d) failed: %s\n",
                cpu, strerror(errno));
    }
}

static void *worker_main(void *opaque) {
    struct thread_ctx *ctx = (struct thread_ctx *)opaque;
    struct shared_state *shared = ctx->shared;
    long i;

    try_pin_to_cpu(ctx->cpu);

    for (i = 0; i < shared->iterations; ++i) {
        pthread_barrier_wait(&shared->iter_barrier);

        if (ctx->is_thread0) {
            store_u64(&shared->x, 1);
            compiler_barrier();
            if (shared->mode == MODE_FENCE_RW_RW) {
                do_fence_rw_rw();
            }
            compiler_barrier();
            shared->r0 = load_u64(&shared->y);
        } else {
            store_u64(&shared->y, 1);
            compiler_barrier();
            if (shared->mode == MODE_FENCE_RW_RW) {
                do_fence_rw_rw();
            }
            compiler_barrier();
            shared->r1 = load_u64(&shared->x);
        }

        pthread_barrier_wait(&shared->iter_barrier);
    }

    return NULL;
}

static double now_seconds(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

static long run_mode(const char *label, int mode, long iterations, int cpu0, int cpu1) {
    struct shared_state shared;
    struct thread_ctx thread0_ctx;
    struct thread_ctx thread1_ctx;
    pthread_t thread0;
    pthread_t thread1;
    long both_zero = 0;
    long i;
    double start_ts;
    double end_ts;

    memset(&shared, 0, sizeof(shared));
    shared.iterations = iterations;
    shared.mode = mode;

    if (pthread_barrier_init(&shared.iter_barrier, NULL, 3) != 0) {
        fprintf(stderr, "[guivp-fence-sb] fail: pthread_barrier_init failed\n");
        return -1;
    }

    thread0_ctx.shared = &shared;
    thread0_ctx.cpu = cpu0;
    thread0_ctx.is_thread0 = 1;
    thread1_ctx.shared = &shared;
    thread1_ctx.cpu = cpu1;
    thread1_ctx.is_thread0 = 0;

    if (pthread_create(&thread0, NULL, worker_main, &thread0_ctx) != 0) {
        fprintf(stderr, "[guivp-fence-sb] fail: pthread_create(thread0) failed\n");
        pthread_barrier_destroy(&shared.iter_barrier);
        return -1;
    }
    if (pthread_create(&thread1, NULL, worker_main, &thread1_ctx) != 0) {
        fprintf(stderr, "[guivp-fence-sb] fail: pthread_create(thread1) failed\n");
        pthread_join(thread0, NULL);
        pthread_barrier_destroy(&shared.iter_barrier);
        return -1;
    }

    start_ts = now_seconds();

    for (i = 0; i < iterations; ++i) {
        shared.x = 0;
        shared.y = 0;
        shared.r0 = 0;
        shared.r1 = 0;
        compiler_barrier();

        pthread_barrier_wait(&shared.iter_barrier);
        pthread_barrier_wait(&shared.iter_barrier);

        if (shared.r0 == 0 && shared.r1 == 0) {
            ++both_zero;
        }
    }

    end_ts = now_seconds();

    pthread_join(thread0, NULL);
    pthread_join(thread1, NULL);
    pthread_barrier_destroy(&shared.iter_barrier);

    printf("[guivp-fence-sb] mode=%s iterations=%ld both_zero=%ld elapsed_sec=%.6f\n",
           label, iterations, both_zero, end_ts - start_ts);
    fflush(stdout);
    return both_zero;
}

int main(int argc, char **argv) {
    long iterations = 20000;
    int cpu_count;
    long no_fence_both_zero;
    long fence_both_zero;

    if (argc >= 2) {
        char *end = NULL;
        iterations = strtol(argv[1], &end, 0);
        if (end == argv[1] || *end != '\0' || iterations <= 0) {
            fprintf(stderr, "usage: %s [iterations]\n", argv[0]);
            return 2;
        }
    }

    cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
    printf("[guivp-fence-sb] cpu_count=%d iterations=%ld\n", cpu_count, iterations);
    if (cpu_count < 2) {
        fprintf(stderr, "[guivp-fence-sb] fail: need at least 2 online CPUs\n");
        return 2;
    }

    no_fence_both_zero = run_mode("no-fence", MODE_NO_FENCE, iterations, 0, 1);
    if (no_fence_both_zero < 0) {
        return 1;
    }

    fence_both_zero = run_mode("fence-rw-rw", MODE_FENCE_RW_RW, iterations, 0, 1);
    if (fence_both_zero < 0) {
        return 1;
    }

    if (fence_both_zero != 0) {
        printf("[guivp-fence-sb] RESULT=FAIL reason=fence-mode-observed-both-zero count=%ld\n",
               fence_both_zero);
        return 1;
    }

    if (no_fence_both_zero == 0) {
        printf("[guivp-fence-sb] RESULT=PASS note=no-fence-did-not-hit-both-zero-in-this-run\n");
        return 0;
    }

    printf("[guivp-fence-sb] RESULT=PASS note=no-fence-hit-both-zero and fence blocked it\n");
    return 0;
}
