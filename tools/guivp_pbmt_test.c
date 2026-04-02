#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_DEVICE "/dev/guivp_pbmt_test"
#define DEFAULT_DT_PATH "/sys/firmware/devicetree/base/reserved-memory/cache-ace@f0000000/reg"
#define FALLBACK_DT_PATH "/proc/device-tree/reserved-memory/cache-ace@f0000000/reg"

enum region_mode {
	REGION_IO,
	REGION_NC,
	REGION_BOTH,
};

struct region_info {
	uint64_t base;
	uint64_t size;
	uint64_t half_size;
};

static void usage(const char *prog) {
	fprintf(stderr,
	        "Usage: %s <io|nc|both> [hex-value]\n"
	        "  io        test the IO-mapped half\n"
	        "  nc        test the NC-mapped half\n"
	        "  both      test both halves sequentially\n"
	        "  hex-value optional 64-bit pattern, e.g. 0x1122334455667788\n",
	        prog);
}

static uint64_t read_be64(const unsigned char *buf) {
	uint64_t value = 0;
	size_t i;

	for (i = 0; i < 8; ++i)
		value = (value << 8) | buf[i];

	return value;
}

static int load_region_info(struct region_info *info) {
	const char *paths[] = {DEFAULT_DT_PATH, FALLBACK_DT_PATH};
	unsigned char buf[16];
	size_t i;

	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
		FILE *fp = fopen(paths[i], "rb");
		size_t nread;

		if (!fp)
			continue;

		nread = fread(buf, 1, sizeof(buf), fp);
		fclose(fp);
		if (nread != sizeof(buf))
			continue;

		info->base = read_be64(buf);
		info->size = read_be64(buf + 8);
		info->half_size = info->size / 2;
		info->half_size -= info->half_size % (uint64_t)sysconf(_SC_PAGESIZE);
		if (info->half_size == 0)
			return -1;
		return 0;
	}

	return -1;
}

static int run_one_test(int fd, const struct region_info *info, enum region_mode region, uint64_t value) {
	const char *name = region == REGION_IO ? "IO" : "NC";
	const uint64_t region_offset = region == REGION_IO ? 0 : info->half_size;
	const uint64_t phys = info->base + region_offset;
	long page_size = sysconf(_SC_PAGESIZE);
	void *mapping;
	volatile uint64_t *ptr;
	uint64_t readback;

	if (page_size <= 0) {
		fprintf(stderr, "invalid page size\n");
		return 1;
	}

	mapping = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)region_offset);
	if (mapping == MAP_FAILED) {
		fprintf(stderr, "%s mmap failed at offset 0x%016" PRIx64 ": %s\n",
		        name, region_offset, strerror(errno));
		return 1;
	}

	ptr = (volatile uint64_t *)mapping;
	*ptr = value;
	readback = *ptr;

	printf("%s region phys=0x%016" PRIx64 " offset=0x%016" PRIx64
	       " wrote=0x%016" PRIx64 " read=0x%016" PRIx64 " => %s\n",
	       name, phys, region_offset, value, readback, readback == value ? "PASS" : "FAIL");

	if (munmap(mapping, (size_t)page_size) != 0) {
		fprintf(stderr, "%s munmap failed: %s\n", name, strerror(errno));
		return 1;
	}

	return readback == value ? 0 : 1;
}

int main(int argc, char **argv) {
	struct region_info info;
	enum region_mode mode;
	uint64_t base_value = 0x5a5aa5a50f0f9696ULL;
	int fd;
	int rc = 0;

	if (argc < 2 || argc > 3) {
		usage(argv[0]);
		return 1;
	}

	if (strcmp(argv[1], "io") == 0) {
		mode = REGION_IO;
	} else if (strcmp(argv[1], "nc") == 0) {
		mode = REGION_NC;
	} else if (strcmp(argv[1], "both") == 0) {
		mode = REGION_BOTH;
	} else {
		usage(argv[0]);
		return 1;
	}

	if (argc == 3) {
		char *endptr = NULL;

		base_value = strtoull(argv[2], &endptr, 0);
		if (!endptr || *endptr != '\0') {
			fprintf(stderr, "invalid test value: %s\n", argv[2]);
			return 1;
		}
	}

	if (load_region_info(&info) != 0) {
		fprintf(stderr, "failed to read cache-ace reserved-memory info from device tree\n");
		return 1;
	}

	printf("cache-ace base=0x%016" PRIx64 " size=0x%016" PRIx64 " half=0x%016" PRIx64 "\n",
	       info.base, info.size, info.half_size);

	fd = open(DEFAULT_DEVICE, O_RDWR | O_SYNC);
	if (fd < 0) {
		fprintf(stderr, "open(%s) failed: %s\n", DEFAULT_DEVICE, strerror(errno));
		return 1;
	}

	if (mode == REGION_IO || mode == REGION_BOTH)
		rc |= run_one_test(fd, &info, REGION_IO, base_value ^ info.base);
	if (mode == REGION_NC || mode == REGION_BOTH)
		rc |= run_one_test(fd, &info, REGION_NC, base_value ^ (info.base + info.half_size));

	close(fd);
	return rc;
}
