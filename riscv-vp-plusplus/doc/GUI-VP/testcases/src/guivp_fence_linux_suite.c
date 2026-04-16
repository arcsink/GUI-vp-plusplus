#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

typedef int (*code_fn_t)(void);

enum sb_mode {
    SB_MODE_NO_FENCE = 0,
    SB_MODE_FENCE_RW_RW = 1,
    SB_MODE_FENCE_TSO = 2,
};

enum publish_fence_kind {
    PUBLISH_FENCE_NONE = 0,
    PUBLISH_FENCE_RW_RW = 1,
    PUBLISH_FENCE_TSO = 2,
};

struct sb_shared_state {
    pthread_barrier_t iter_barrier;
    volatile uint64_t x __attribute__((aligned(64)));
    volatile uint64_t y __attribute__((aligned(64)));
    volatile uint64_t r0 __attribute__((aligned(64)));
    volatile uint64_t r1 __attribute__((aligned(64)));
    long iterations;
    int mode;
};

struct sb_thread_ctx {
    struct sb_shared_state *shared;
    int cpu;
    int is_thread0;
};

struct remote_i_shared {
    pthread_barrier_t ready_barrier;
    volatile int phase;
    volatile int result;
    volatile int warmup_result;
    int consumer_cpu;
    int do_consumer_fence_i;
    code_fn_t fn;
};

static inline void compiler_barrier(void) {
    __asm__ volatile("" ::: "memory");
}

static inline void do_fence_rw_rw(void) {
    __asm__ volatile("fence rw,rw" ::: "memory");
}

static inline void do_fence_i(void) {
    __asm__ volatile("fence.i" ::: "memory");
}

static inline void do_fence_tso(void) {
    __asm__ volatile(".4byte 0x8330000f" ::: "memory");
}

static inline void store_u64(volatile uint64_t *ptr, uint64_t value) {
    __asm__ volatile("sd %1, 0(%0)" :: "r"(ptr), "r"(value) : "memory");
}

static inline uint64_t load_u64(volatile uint64_t *ptr) {
    uint64_t value;
    __asm__ volatile("ld %0, 0(%1)" : "=r"(value) : "r"(ptr) : "memory");
    return value;
}

static void try_pin_to_cpu(int cpu) {
    cpu_set_t mask;

    if (cpu < 0) {
        return;
    }

    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
        fprintf(stderr, "[guivp-fence-suite] warn: sched_setaffinity(cpu=%d) failed: %s\n",
                cpu, strerror(errno));
    }
}

static double now_seconds(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

static void emit_return_imm(code_fn_t *fn_out, uint32_t *code, int imm) {
    code[0] = (((uint32_t)imm) & 0xfffU) << 20 | (10U << 7) | 0x13U;
    code[1] = 0x00008067U;
    compiler_barrier();
    *fn_out = (code_fn_t)code;
}

static uint32_t *alloc_exec_page(void) {
    void *ptr;
    const long page_size = sysconf(_SC_PAGESIZE);

    ptr = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE | PROT_EXEC,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        return NULL;
    }
    memset(ptr, 0, (size_t)page_size);
    return (uint32_t *)ptr;
}

static void free_exec_page(uint32_t *code) {
    const long page_size = sysconf(_SC_PAGESIZE);
    if (code != NULL) {
        munmap(code, (size_t)page_size);
    }
}

static inline void maybe_issue_sb_fence(int mode) {
    if (mode == SB_MODE_FENCE_RW_RW) {
        do_fence_rw_rw();
    } else if (mode == SB_MODE_FENCE_TSO) {
        do_fence_tso();
    }
}

static void *sb_worker_main(void *opaque) {
    struct sb_thread_ctx *ctx = (struct sb_thread_ctx *)opaque;
    struct sb_shared_state *shared = ctx->shared;
    long i;

    try_pin_to_cpu(ctx->cpu);

    for (i = 0; i < shared->iterations; ++i) {
        pthread_barrier_wait(&shared->iter_barrier);

        if (ctx->is_thread0) {
            store_u64(&shared->x, 1);
            compiler_barrier();
            maybe_issue_sb_fence(shared->mode);
            compiler_barrier();
            shared->r0 = load_u64(&shared->y);
        } else {
            store_u64(&shared->y, 1);
            compiler_barrier();
            maybe_issue_sb_fence(shared->mode);
            compiler_barrier();
            shared->r1 = load_u64(&shared->x);
        }

        pthread_barrier_wait(&shared->iter_barrier);
    }

    return NULL;
}

static long run_sb_mode(const char *label, int mode, long iterations, int cpu0, int cpu1) {
    struct sb_shared_state shared;
    struct sb_thread_ctx thread0_ctx;
    struct sb_thread_ctx thread1_ctx;
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
        fprintf(stderr, "[guivp-fence-suite] fail: pthread_barrier_init failed\n");
        return -1;
    }

    thread0_ctx.shared = &shared;
    thread0_ctx.cpu = cpu0;
    thread0_ctx.is_thread0 = 1;
    thread1_ctx.shared = &shared;
    thread1_ctx.cpu = cpu1;
    thread1_ctx.is_thread0 = 0;

    if (pthread_create(&thread0, NULL, sb_worker_main, &thread0_ctx) != 0) {
        fprintf(stderr, "[guivp-fence-suite] fail: pthread_create(thread0) failed\n");
        pthread_barrier_destroy(&shared.iter_barrier);
        return -1;
    }
    if (pthread_create(&thread1, NULL, sb_worker_main, &thread1_ctx) != 0) {
        fprintf(stderr, "[guivp-fence-suite] fail: pthread_create(thread1) failed\n");
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

    printf("[guivp-fence-suite] sb mode=%s iterations=%ld both_zero=%ld elapsed_sec=%.6f\n",
           label, iterations, both_zero, end_ts - start_ts);
    fflush(stdout);
    return both_zero;
}

static int run_local_fence_i_test(void) {
    uint32_t *code = alloc_exec_page();
    code_fn_t fn = NULL;
    int warmup_ret;
    int no_fence_ret;
    int with_fence_ret;
    int i;

    if (code == NULL) {
        fprintf(stderr, "[guivp-fence-suite] fail: mmap exec page failed: %s\n", strerror(errno));
        return 1;
    }

    emit_return_imm(&fn, code, 1);
    for (i = 0; i < 16; ++i) {
        warmup_ret = fn();
        (void)warmup_ret;
    }

    emit_return_imm(&fn, code, 2);
    no_fence_ret = fn();
    do_fence_i();
    with_fence_ret = fn();

    printf("[guivp-fence-suite] fencei-local warm=1 no_fence_ret=%d with_fence_i_ret=%d\n",
           no_fence_ret, with_fence_ret);
    fflush(stdout);

    free_exec_page(code);

    if (no_fence_ret != 1) {
        fprintf(stderr, "[guivp-fence-suite] fail: local no-fence.i expected stale 1, got %d\n",
                no_fence_ret);
        return 1;
    }
    if (with_fence_ret != 2) {
        fprintf(stderr, "[guivp-fence-suite] fail: local fence.i expected 2, got %d\n",
                with_fence_ret);
        return 1;
    }

    return 0;
}

static void *remote_i_consumer_main(void *opaque) {
    struct remote_i_shared *shared = (struct remote_i_shared *)opaque;
    int i;
    int warmup = 0;

    try_pin_to_cpu(shared->consumer_cpu);

    for (i = 0; i < 32; ++i) {
        warmup = shared->fn();
    }
    shared->warmup_result = warmup;

    pthread_barrier_wait(&shared->ready_barrier);

    while (shared->phase == 0) {
    }

    if (shared->do_consumer_fence_i) {
        do_fence_i();
    }

    shared->result = shared->fn();
    return NULL;
}

static void maybe_issue_publish_fence(int kind) {
    if (kind == PUBLISH_FENCE_RW_RW) {
        do_fence_rw_rw();
    } else if (kind == PUBLISH_FENCE_TSO) {
        do_fence_tso();
    }
}

static int run_remote_fence_i_round(const char *label, int publish_fence_kind, int consumer_fence_i,
                                    int new_imm, int expected_ret) {
    struct remote_i_shared shared;
    pthread_t consumer_thread;
    uint32_t *code = alloc_exec_page();
    code_fn_t fn = NULL;
    int rc = 1;

    if (code == NULL) {
        fprintf(stderr, "[guivp-fence-suite] fail: mmap exec page failed: %s\n", strerror(errno));
        return 1;
    }

    memset(&shared, 0, sizeof(shared));
    shared.consumer_cpu = 1;
    shared.do_consumer_fence_i = consumer_fence_i;
    emit_return_imm(&fn, code, 1);
    shared.fn = fn;

    if (pthread_barrier_init(&shared.ready_barrier, NULL, 2) != 0) {
        fprintf(stderr, "[guivp-fence-suite] fail: pthread_barrier_init remote failed\n");
        free_exec_page(code);
        return 1;
    }

    if (pthread_create(&consumer_thread, NULL, remote_i_consumer_main, &shared) != 0) {
        fprintf(stderr, "[guivp-fence-suite] fail: pthread_create consumer failed\n");
        pthread_barrier_destroy(&shared.ready_barrier);
        free_exec_page(code);
        return 1;
    }

    try_pin_to_cpu(0);
    pthread_barrier_wait(&shared.ready_barrier);

    emit_return_imm(&fn, code, new_imm);
    shared.fn = fn;
    compiler_barrier();
    maybe_issue_publish_fence(publish_fence_kind);
    compiler_barrier();
    shared.phase = 1;

    pthread_join(consumer_thread, NULL);

    printf("[guivp-fence-suite] fencei-remote label=%s warmup=%d consumer_fence_i=%d result=%d expected=%d\n",
           label, shared.warmup_result, consumer_fence_i, shared.result, expected_ret);
    fflush(stdout);

    if (shared.warmup_result != 1) {
        fprintf(stderr, "[guivp-fence-suite] fail: remote warmup expected 1, got %d\n",
                shared.warmup_result);
        goto out;
    }
    if (shared.result != expected_ret) {
        fprintf(stderr, "[guivp-fence-suite] fail: remote %s expected %d, got %d\n",
                label, expected_ret, shared.result);
        goto out;
    }

    rc = 0;

out:
    pthread_barrier_destroy(&shared.ready_barrier);
    free_exec_page(code);
    return rc;
}

int main(int argc, char **argv) {
    long iterations = 2000;
    int cpu_count;
    long no_fence_both_zero;
    long rwrw_both_zero;
    long tso_both_zero;
    int failures = 0;

    if (argc >= 2) {
        char *end = NULL;
        iterations = strtol(argv[1], &end, 0);
        if (end == argv[1] || *end != '\0' || iterations <= 0) {
            fprintf(stderr, "usage: %s [iterations]\n", argv[0]);
            return 2;
        }
    }

    cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
    printf("[guivp-fence-suite] cpu_count=%d sb_iterations=%ld\n", cpu_count, iterations);
    if (cpu_count < 2) {
        fprintf(stderr, "[guivp-fence-suite] fail: need at least 2 online CPUs\n");
        return 2;
    }

    no_fence_both_zero = run_sb_mode("no-fence", SB_MODE_NO_FENCE, iterations, 0, 1);
    rwrw_both_zero = run_sb_mode("fence-rw-rw", SB_MODE_FENCE_RW_RW, iterations, 0, 1);
    tso_both_zero = run_sb_mode("fence-tso", SB_MODE_FENCE_TSO, iterations, 0, 1);
    if (no_fence_both_zero < 0 || rwrw_both_zero < 0 || tso_both_zero < 0) {
        return 1;
    }

    if (rwrw_both_zero != 0) {
        fprintf(stderr, "[guivp-fence-suite] fail: fence-rw-rw observed both_zero=%ld\n",
                rwrw_both_zero);
        failures++;
    }
    if (tso_both_zero != 0) {
        fprintf(stderr, "[guivp-fence-suite] fail: fence-tso observed both_zero=%ld\n",
                tso_both_zero);
        failures++;
    }

    failures += run_local_fence_i_test();
    failures += run_remote_fence_i_round("no-remote-fence-i", PUBLISH_FENCE_RW_RW, 0, 2, 1);
    failures += run_remote_fence_i_round("remote-fence-i-rw-rw-publish", PUBLISH_FENCE_RW_RW, 1, 3, 3);
    failures += run_remote_fence_i_round("remote-fence-i-tso-publish", PUBLISH_FENCE_TSO, 1, 4, 4);

    printf("[guivp-fence-suite] summary sb_no_fence_both_zero=%ld sb_fence_rwrw_both_zero=%ld sb_fence_tso_both_zero=%ld failures=%d\n",
           no_fence_both_zero, rwrw_both_zero, tso_both_zero, failures);
    if (failures != 0) {
        printf("[guivp-fence-suite] RESULT=FAIL\n");
        return 1;
    }

    if (no_fence_both_zero == 0) {
        printf("[guivp-fence-suite] RESULT=PASS note=sb-no-fence-did-not-hit-both-zero-in-this-run\n");
    } else {
        printf("[guivp-fence-suite] RESULT=PASS note=sb-no-fence-hit-both-zero-and-fenced-modes-blocked-it\n");
    }
    return 0;
}
