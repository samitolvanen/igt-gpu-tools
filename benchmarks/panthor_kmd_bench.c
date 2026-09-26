// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (C) 2026 Google LLC

/*
 * panthor_kmd_bench - CPU cost of the Panthor uAPI ioctls in the kernel
 * driver, Panthor or Tyr.
 *
 * Each benchmark runs a warm-up, then --reps repetitions of --iters calls,
 * and prints one TSV row per repetition to stdout:
 *
 *   bench driver rep ops wall_ns instr_k cycles_k
 *
 * ops is the number of operations a repetition counts; wall_ns is the
 * CLOCK_MONOTONIC time per operation. instr_k and cycles_k are the kernel
 * instructions and CPU cycles per operation, in thousands, counted on the
 * calling thread only: the work the driver runs on workqueues and IRQ
 * threads is not included. They print as "n/a" when the counters cannot be
 * opened.
 *
 * The median, IQR and minimum of each column over the repetitions go to
 * stderr after each benchmark.
 *
 * Pin the process to one CPU with --cpu: on a system with more than one
 * CPU type, the counters then come from the PMU of that CPU.
 */

#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <linux/perf_event.h>

#include "drm.h"
#include "drmtest.h"
#include "igt_panthor.h"
#include "igt_stats.h"
#include "igt_syncobj.h"
#include "ioctl_wrappers.h"
#include "panthor_drm.h"

#define CS_VA 0x200000ULL
#define DST_VA 0x400000ULL
#define BIND_VA 0x10000000ULL
#define BIND_2M_VA 0x20000000ULL
#define USER_VA_RANGE 0x100000000ULL
#define MANY 64
#define BATCH 16

/* Armv8 PMU common events. */
#define ARMV8_INST_RETIRED 0x08
#define ARMV8_CPU_CYCLES 0x11

static struct {
	int fd;
	uint32_t vm_id;
	uint32_t group;
	uint32_t cs_bo, dst_bo, bo_4k, bo_2m;
	uint32_t latest_flush;
	uint32_t sync_out, sync_signaled;
	char driver[32];
	int perf_fd;
} g = { .perf_fd = -1 };

static void xioctl(unsigned long req, void *arg, const char *what)
{
	if (igt_ioctl(g.fd, req, arg)) {
		fprintf(stderr, "panthor_kmd_bench: %s: %s\n", what, strerror(errno));
		exit(1);
	}
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Counters */

static int perf_open(struct perf_event_attr *attr, int group_fd)
{
	return syscall(__NR_perf_event_open, attr, 0, -1, group_fd, 0);
}

static bool cpulist_has(const char *list, int cpu)
{
	const char *p = list;

	while (*p) {
		char *end;
		long lo = strtol(p, &end, 10), hi = lo;

		if (end == p)
			return false;
		if (*end == '-')
			hi = strtol(end + 1, &end, 10);
		if (cpu >= lo && cpu <= hi)
			return true;
		if (*end != ',')
			return false;
		p = end + 1;
	}
	return false;
}

/*
 * The perf type of the CPU PMU that covers @cpu, or -1. Only a system with
 * more than one CPU PMU lists the CPUs of each.
 */
static int cpu_pmu_type(int cpu, char *name, size_t len)
{
	static const char *const pmus[] = { "armv8_", "armv9_", "arm_" };
	char path[PATH_MAX], buf[256];
	struct dirent *de;
	int type = -1;
	DIR *dir;

	dir = opendir("/sys/bus/event_source/devices");
	if (!dir)
		return -1;
	while (type < 0 && (de = readdir(dir))) {
		bool arm = false;
		FILE *f;

		for (size_t i = 0; i < ARRAY_SIZE(pmus); i++)
			arm |= !strncmp(de->d_name, pmus[i], strlen(pmus[i]));
		if (!arm)
			continue;
		snprintf(path, sizeof(path), "/sys/bus/event_source/devices/%s/cpus", de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(buf, sizeof(buf), f) && cpulist_has(buf, cpu)) {
			FILE *t;

			snprintf(path, sizeof(path), "/sys/bus/event_source/devices/%s/type",
				 de->d_name);
			t = fopen(path, "r");
			if (t) {
				if (fscanf(t, "%d", &type) != 1)
					type = -1;
				fclose(t);
			}
			snprintf(name, len, "%s", de->d_name);
		}
		fclose(f);
	}
	closedir(dir);
	return type;
}

/* The CPU the process is pinned to, or -1. */
static int pinned_cpu(void)
{
	cpu_set_t set;

	if (sched_getaffinity(0, sizeof(set), &set) || CPU_COUNT(&set) != 1)
		return -1;
	for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
		if (CPU_ISSET(cpu, &set))
			return cpu;
	return -1;
}

/*
 * Opens kernel-mode instructions and cycles on the calling thread as one
 * group. On a CPU with its own PMU, the Armv8 common events of that PMU:
 * a generic hardware event would count on one CPU type only.
 */
static void perf_setup(void)
{
	struct perf_event_attr attr = {
		.size = sizeof(attr),
		.exclude_user = 1,
		.exclude_hv = 1,
		.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
			       PERF_FORMAT_TOTAL_TIME_RUNNING,
	};
	uint64_t instr = PERF_COUNT_HW_INSTRUCTIONS, cycles = PERF_COUNT_HW_CPU_CYCLES;
	int cpu = pinned_cpu(), type = -1, fd;
	char pmu[NAME_MAX + 1] = "generic";

	attr.type = PERF_TYPE_HARDWARE;
	if (cpu >= 0)
		type = cpu_pmu_type(cpu, pmu, sizeof(pmu));
	if (type >= 0) {
		attr.type = type;
		instr = ARMV8_INST_RETIRED;
		cycles = ARMV8_CPU_CYCLES;
	}

	attr.config = instr;
	g.perf_fd = perf_open(&attr, -1);
	if (g.perf_fd < 0) {
		fprintf(stderr, "# perf: %s: %s, instr_k and cycles_k are n/a\n", pmu,
			strerror(errno));
		return;
	}
	attr.config = cycles;
	fd = perf_open(&attr, g.perf_fd);
	if (fd < 0) {
		fprintf(stderr, "# perf: %s cycles: %s, instr_k and cycles_k are n/a\n", pmu,
			strerror(errno));
		close(g.perf_fd);
		g.perf_fd = -1;
		return;
	}
	fprintf(stderr, "# perf: kernel instructions and cycles from the %s PMU", pmu);
	if (cpu >= 0)
		fprintf(stderr, ", CPU %d", cpu);
	fprintf(stderr, "\n");
}

struct counts {
	uint64_t instr, cycles;
	bool valid;
};

static struct counts perf_read(void)
{
	struct { uint64_t nr, enabled, running, v[2]; } r;
	struct counts c = {};

	if (g.perf_fd < 0)
		return c;
	if (read(g.perf_fd, &r, sizeof(r)) != sizeof(r) || r.nr != 2)
		return c;
	/* A multiplexed group counts part of the time only. */
	if (r.running != r.enabled)
		return c;
	c.instr = r.v[0];
	c.cycles = r.v[1];
	c.valid = true;
	return c;
}

/* Setup */

static uint32_t bo_create(uint64_t size, uint32_t vm)
{
	struct drm_panthor_bo_create c = { .size = size, .exclusive_vm_id = vm };

	do_ioctl(g.fd, DRM_IOCTL_PANTHOR_BO_CREATE, &c);
	return c.handle;
}

static void vm_bind(struct drm_panthor_vm_bind_op *ops, uint32_t count, bool async,
		    uint32_t signal)
{
	struct drm_panthor_sync_op so = {
		.flags = DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ | DRM_PANTHOR_SYNC_OP_SIGNAL,
		.handle = signal,
	};
	struct drm_panthor_vm_bind vb = {
		.vm_id = g.vm_id,
		.flags = async ? DRM_PANTHOR_VM_BIND_ASYNC : 0,
		.ops = DRM_PANTHOR_OBJ_ARRAY(count, ops),
	};

	if (async)
		ops[count - 1].syncs = (struct drm_panthor_obj_array)DRM_PANTHOR_OBJ_ARRAY(1, &so);
	xioctl(DRM_IOCTL_PANTHOR_VM_BIND, &vb, "VM_BIND");
}

static void map_one(uint32_t bo, uint64_t va, uint64_t size, bool async, uint32_t signal)
{
	struct drm_panthor_vm_bind_op op = {
		.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
		.bo_handle = bo,
		.va = va,
		.size = size,
	};

	vm_bind(&op, 1, async, signal);
}

static void unmap_one(uint64_t va, uint64_t size, bool async, uint32_t signal)
{
	struct drm_panthor_vm_bind_op op = {
		.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP,
		.va = va,
		.size = size,
	};

	vm_bind(&op, 1, async, signal);
}

static void wait_one(uint32_t handle)
{
	struct drm_syncobj_wait w = {
		.handles = to_user_pointer(&handle),
		.count_handles = 1,
		.timeout_nsec = INT64_MAX,
	};

	xioctl(DRM_IOCTL_SYNCOBJ_WAIT, &w, "SYNCOBJ_WAIT");
}

/* One job: MOV32 r2, #1. The stream does no memory access. */
static void submit(uint32_t wait_on)
{
	struct drm_panthor_sync_op so[2] = {
		{
			.flags = DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ |
				 DRM_PANTHOR_SYNC_OP_SIGNAL,
			.handle = g.sync_out,
		},
		{
			.flags = DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ |
				 DRM_PANTHOR_SYNC_OP_WAIT,
			.handle = wait_on,
		},
	};
	struct drm_panthor_queue_submit qs = {
		.queue_index = 0,
		.stream_size = sizeof(uint64_t),
		.stream_addr = CS_VA,
		.latest_flush = g.latest_flush,
		.syncs = DRM_PANTHOR_OBJ_ARRAY(wait_on ? 2 : 1, so),
	};
	struct drm_panthor_group_submit gs = {
		.group_handle = g.group,
		.queue_submits = DRM_PANTHOR_OBJ_ARRAY(1, &qs),
	};

	xioctl(DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs, "GROUP_SUBMIT");
}

/*
 * The driver column: the DRM name is "panthor" for both drivers, so Tyr is
 * told apart by its description.
 */
static void driver_name(void)
{
	char name[32] = "", desc[128] = "", date[32] = "";
	struct drm_version v = {
		.name_len = sizeof(name) - 1,
		.name = name,
		.date_len = sizeof(date) - 1,
		.date = date,
		.desc_len = sizeof(desc) - 1,
		.desc = desc,
	};

	do_ioctl(g.fd, DRM_IOCTL_VERSION, &v);
	snprintf(g.driver, sizeof(g.driver), "%s", strcasestr(desc, "tyr") ? "tyr" : name);
	fprintf(stderr, "# driver: %s %d.%d.%d (%s)\n", name, v.version_major,
		v.version_minor, v.version_patchlevel, desc);
}

static void setup(void)
{
	struct drm_panthor_vm_create vc = { .user_va_range = USER_VA_RANGE };
	struct drm_panthor_queue_create qc = { .priority = 1, .ringbuf_size = 65536 };
	struct drm_panthor_group_create gc = {};
	struct drm_panthor_vm_bind_op ops[2];
	struct drm_panthor_gpu_info gi = {};
	volatile uint32_t *flush_id;
	uint64_t *cs;

	g.fd = drm_open_driver_render(DRIVER_PANTHOR);
	driver_name();

	do_ioctl(g.fd, DRM_IOCTL_PANTHOR_VM_CREATE, &vc);
	g.vm_id = vc.id;

	g.cs_bo = bo_create(65536, g.vm_id);
	g.dst_bo = bo_create(65536, g.vm_id);
	cs = igt_panthor_mmap_bo(g.fd, g.cs_bo, 65536, PROT_READ | PROT_WRITE,
				 igt_panthor_bo_mmap_offset(g.fd, g.cs_bo, 0));
	cs[0] = cs_mov32(2, 1);
	munmap(cs, 65536);

	/*
	 * The flush ID at setup, for every job: a job then flushes the caches
	 * as it starts, and the loop does not read the register page.
	 */
	flush_id = mmap(NULL, 4096, PROT_READ, MAP_SHARED, g.fd,
			DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET);
	igt_assert(flush_id != MAP_FAILED);
	g.latest_flush = flush_id[0];
	munmap((void *)flush_id, 4096);

	memset(ops, 0, sizeof(ops));
	ops[0].flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP;
	ops[0].bo_handle = g.cs_bo;
	ops[0].va = CS_VA;
	ops[0].size = 65536;
	ops[1].flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP;
	ops[1].bo_handle = g.dst_bo;
	ops[1].va = DST_VA;
	ops[1].size = 65536;
	vm_bind(ops, 2, false, 0);

	g.bo_4k = bo_create(4096, g.vm_id);
	g.bo_2m = bo_create(2 << 20, g.vm_id);

	igt_panthor_query(g.fd, DRM_PANTHOR_DEV_QUERY_GPU_INFO, &gi, sizeof(gi), 0);
	gc.queues = (struct drm_panthor_obj_array)DRM_PANTHOR_OBJ_ARRAY(1, &qc);
	gc.compute_core_mask = gi.shader_present;
	gc.fragment_core_mask = gi.shader_present;
	gc.tiler_core_mask = gi.tiler_present;
	gc.max_compute_cores = __builtin_popcountll(gi.shader_present);
	gc.max_fragment_cores = __builtin_popcountll(gi.shader_present);
	gc.max_tiler_cores = __builtin_popcountll(gi.tiler_present);
	gc.priority = PANTHOR_GROUP_PRIORITY_MEDIUM;
	gc.vm_id = g.vm_id;
	igt_panthor_group_create(g.fd, &gc, 0);
	g.group = gc.group_handle;

	g.sync_out = syncobj_create(g.fd, 0);
	g.sync_signaled = syncobj_create(g.fd, DRM_SYNCOBJ_CREATE_SIGNALED);
}

/* Benchmarks. Each runs one call; the unit is in the table below. */

static void b_query(void)
{
	static struct drm_panthor_gpu_info gi;
	struct drm_panthor_dev_query q = {
		.type = DRM_PANTHOR_DEV_QUERY_GPU_INFO,
		.size = sizeof(gi),
		.pointer = to_user_pointer(&gi),
	};

	xioctl(DRM_IOCTL_PANTHOR_DEV_QUERY, &q, "DEV_QUERY");
}

static void b_submit_wait(void)
{
	submit(0);
	wait_one(g.sync_out);
}

static void b_submit_batch(void)
{
	for (int i = 0; i < BATCH; i++)
		submit(0);
	wait_one(g.sync_out);
}

static void b_submit_dep(void)
{
	submit(g.sync_signaled);
	wait_one(g.sync_out);
}

static void b_bind_4k(void)
{
	map_one(g.bo_4k, BIND_VA, 4096, false, 0);
	unmap_one(BIND_VA, 4096, false, 0);
}

static void b_bind_2m(void)
{
	map_one(g.bo_2m, BIND_2M_VA, 2 << 20, false, 0);
	unmap_one(BIND_2M_VA, 2 << 20, false, 0);
}

static void b_bind_64x4k(void)
{
	struct drm_panthor_vm_bind_op ops[MANY];

	memset(ops, 0, sizeof(ops));
	for (int i = 0; i < MANY; i++) {
		ops[i].flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP;
		ops[i].bo_handle = g.bo_4k;
		ops[i].va = BIND_VA + (uint64_t)i * 4096;
		ops[i].size = 4096;
	}
	vm_bind(ops, MANY, false, 0);
	memset(ops, 0, sizeof(ops));
	for (int i = 0; i < MANY; i++) {
		ops[i].flags = DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP;
		ops[i].va = BIND_VA + (uint64_t)i * 4096;
		ops[i].size = 4096;
	}
	vm_bind(ops, MANY, false, 0);
}

static void b_bind_4k_async(void)
{
	map_one(g.bo_4k, BIND_VA, 4096, true, g.sync_out);
	unmap_one(BIND_VA, 4096, true, g.sync_out);
	wait_one(g.sync_out);
}

static void b_bo_create(void)
{
	struct drm_panthor_bo_create c = { .size = 4096 };
	struct drm_panthor_bo_mmap_offset o = {};
	struct drm_gem_close cl = {};

	xioctl(DRM_IOCTL_PANTHOR_BO_CREATE, &c, "BO_CREATE");
	o.handle = c.handle;
	xioctl(DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &o, "BO_MMAP_OFFSET");
	cl.handle = c.handle;
	xioctl(DRM_IOCTL_GEM_CLOSE, &cl, "GEM_CLOSE");
}

static const struct bench {
	const char *name;
	void (*fn)(void);
	int ops; /* operations one call counts as */
	const char *unit;
} benches[] = {
	{ "query", b_query, 1, "DEV_QUERY(GPU_INFO): the ioctl floor" },
	{ "submit_wait", b_submit_wait, 1, "GROUP_SUBMIT of one job + SYNCOBJ_WAIT" },
	{ "submit_batch", b_submit_batch, BATCH, "one GROUP_SUBMIT of a 16-deep batch, one wait" },
	{ "submit_dep", b_submit_dep, 1, "submit_wait with a signaled syncobj dependency" },
	{ "bind_4k", b_bind_4k, 1, "sync VM_BIND map + unmap of a 4 KiB BO" },
	{ "bind_2m", b_bind_2m, 1, "sync VM_BIND map + unmap of a 2 MiB BO" },
	{ "bind_64x4k", b_bind_64x4k, 1, "one 64-op map VM_BIND + one 64-op unmap VM_BIND" },
	{ "bind_4k_async", b_bind_4k_async, 1, "async map + unmap of 4 KiB, then wait" },
	{ "bo_create", b_bo_create, 1, "BO_CREATE 4 KiB + MMAP_OFFSET + GEM_CLOSE" },
};

/* Statistics */

struct column {
	const char *name;
	igt_stats_t stats;
	double min;
};

static void column_push(struct column *c, double v)
{
	if (!c->stats.n_values || v < c->min)
		c->min = v;
	igt_stats_push_float(&c->stats, v);
}

static void column_print(const struct bench *b, struct column *c)
{
	if (!c->stats.n_values)
		fprintf(stderr, "%-14s %-8s %10s %10s %10s\n", b->name, c->name, "n/a", "n/a",
			"n/a");
	else
		fprintf(stderr, "%-14s %-8s %10.1f %10.1f %10.1f\n", b->name, c->name,
			igt_stats_get_median(&c->stats), igt_stats_get_iqr(&c->stats), c->min);
	igt_stats_fini(&c->stats);
}

static void run(const struct bench *b, int iters, int reps, int warmup,
		struct column cols[3])
{
	double n = (double)iters * b->ops;

	for (int i = 0; i < 3; i++)
		igt_stats_init_with_size(&cols[i].stats, reps);

	for (int i = 0; i < warmup; i++)
		b->fn();

	for (int rep = 0; rep < reps; rep++) {
		struct counts c0, c1;
		uint64_t t0, t1;

		c0 = perf_read();
		t0 = now_ns();
		for (int i = 0; i < iters; i++)
			b->fn();
		t1 = now_ns();
		c1 = perf_read();

		printf("%s\t%s\t%d\t%.0f\t%.1f", b->name, g.driver, rep, n,
		       n ? (t1 - t0) / n : 0.0);
		if (n)
			column_push(&cols[0], (t1 - t0) / n);
		if (c0.valid && c1.valid && n) {
			double instr = (c1.instr - c0.instr) / n / 1000.0;
			double cycles = (c1.cycles - c0.cycles) / n / 1000.0;

			printf("\t%.3f\t%.3f\n", instr, cycles);
			column_push(&cols[1], instr);
			column_push(&cols[2], cycles);
		} else {
			printf("\tn/a\tn/a\n");
		}
		fflush(stdout);
	}
}

static bool selected(const char *only, const char *name)
{
	char list[512], pat[64];

	if (!only)
		return true;
	snprintf(list, sizeof(list), ",%s,", only);
	snprintf(pat, sizeof(pat), ",%s,", name);
	return strstr(list, pat);
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [--iters N] [--reps R] [--warmup W] [--bench a,b,...] [--cpu N] [--list]\n",
		argv0);
}

int main(int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "iters", required_argument, NULL, 'i' },
		{ "reps", required_argument, NULL, 'r' },
		{ "warmup", required_argument, NULL, 'w' },
		{ "bench", required_argument, NULL, 'b' },
		{ "cpu", required_argument, NULL, 'c' },
		{ "list", no_argument, NULL, 'l' },
		{ "help", no_argument, NULL, 'h' },
		{}
	};
	int iters = 200, reps = 5, warmup = 20, cpu = -1, opt;
	const char *only = NULL;

	while ((opt = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
		switch (opt) {
		case 'i':
			iters = atoi(optarg);
			break;
		case 'r':
			reps = atoi(optarg);
			break;
		case 'w':
			warmup = atoi(optarg);
			break;
		case 'b':
			only = optarg;
			break;
		case 'c':
			cpu = atoi(optarg);
			break;
		case 'l':
			for (size_t k = 0; k < ARRAY_SIZE(benches); k++)
				printf("%-14s %s\n", benches[k].name, benches[k].unit);
			return 0;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 2;
		}
	}
	if (optind < argc || iters < 0 || reps < 1 || warmup < 0) {
		usage(argv[0]);
		return 2;
	}

	if (cpu >= 0) {
		cpu_set_t set;

		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		if (sched_setaffinity(0, sizeof(set), &set)) {
			fprintf(stderr, "panthor_kmd_bench: CPU %d: %s\n", cpu, strerror(errno));
			return 1;
		}
	}

	setup();
	perf_setup();

	printf("bench\tdriver\trep\tops\twall_ns\tinstr_k\tcycles_k\n");
	fflush(stdout);
	fprintf(stderr, "%-14s %-8s %10s %10s %10s\n", "bench", "column", "median", "iqr", "min");
	for (size_t k = 0; k < ARRAY_SIZE(benches); k++) {
		struct column cols[3] = {
			{ .name = "wall_ns" }, { .name = "instr_k" }, { .name = "cycles_k" },
		};

		if (!selected(only, benches[k].name))
			continue;
		run(&benches[k], iters, reps, warmup, cols);
		for (int i = 0; i < 3; i++)
			column_print(&benches[k], &cols[i]);
	}

	if (g.perf_fd >= 0)
		close(g.perf_fd);
	drm_close_driver(g.fd);
	return 0;
}
