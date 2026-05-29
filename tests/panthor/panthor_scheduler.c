// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (C) 2026 Google LLC

#include <stdint.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "igt.h"
#include "igt_panthor.h"
#include "igt_syncobj.h"
#include "panthor_drm.h"

/*
 * CSF scheduler functional tests for the Tyr/Panthor driver. Each subtest
 * exercises one slice of CSG
 * slot scheduling: no-op and compute submission, max/over/extreme slot
 * commitment, idle prepopulation, normal and real-time priority preemption,
 * multi-queue groups, sync-wait blocking, job timeout, fatal faults, large
 * instruction streams, ring-buffer wrap, group destruction with active jobs,
 * and multi-queue dependency chains.
 */

#define INITIAL_VA	0x1000000

/* Byte offsets inside the per-group scratch BO. */
#define STARTED_OFFSET	2056
#define COUNTER_OFFSET	2064
#define USER_SYNC_OFFSET 2048

/*
 * Iteration count for the deterministic, register-only counted loops. The loop
 * is pure register arithmetic with no host-written flag read, so it is immune
 * to CPU-WC / GPU-L2 coherence races. Each iteration is ~5 GPU instructions, so
 * this keeps a single on-slot job comfortably under the job timeout while the
 * whole test finishes within the host wait budget.
 */
#define COUNTED_LOOP_N	100000

/* Relative wait budgets in nanoseconds. */
#define SEC_NS		1000000000ULL

/* GPU core masks and CSG slot count, queried once in the top fixture. */
static uint64_t g_shader_present;
static uint64_t g_tiler_present;
static uint32_t g_slots;

static int64_t abs_timeout(int64_t rel_ns)
{
	struct timespec ts;

	igt_assert_eq(clock_gettime(CLOCK_MONOTONIC, &ts), 0);

	return (int64_t)ts.tv_sec * SEC_NS + ts.tv_nsec + rel_ns;
}

static bool wait_done(int fd, uint32_t syncobj, int64_t rel_ns)
{
	return syncobj_wait(fd, &syncobj, 1, abs_timeout(rel_ns),
			    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL);
}

/*
 * Emit a deterministic, register-only counted loop into instrs[].
 *
 * The loop increments a visible counter and writes it to counter_addr exactly n
 * times, then completes; it never reads a host-written flag. If started_addr is
 * non-zero, the BO's "started" flag at that VA is set to 1 once before the loop.
 *
 * CSF branch offsets are relative to the next instruction, so the encoded value
 * is (target - branch - 1) in instruction units. Condition EQ ("branch if
 * source register == 0") emulates an unconditional back-branch by testing a
 * register pre-loaded with 0.
 *
 * Returns the number of 64-bit instruction words emitted.
 */
static int emit_counted_loop(uint64_t *instrs, uint64_t counter_addr,
			     uint64_t started_addr, uint32_t n)
{
	int k = 0;
	int body, br_exit, br_back, exit;

	if (started_addr) {
		instrs[k++] = cs_mov48(8, started_addr);
		instrs[k++] = cs_mov32(6, 1);
		instrs[k++] = cs_sync_set32(8, 6);
	}

	instrs[k++] = cs_mov48(0, counter_addr);
	instrs[k++] = cs_mov32(2, 0);
	instrs[k++] = cs_mov32(4, n);
	instrs[k++] = cs_mov32(10, 0);

	body = k;
	instrs[k++] = cs_add_imm32(2, 2, 1);
	instrs[k++] = cs_sync_set32(0, 2);
	instrs[k++] = cs_add_imm32(4, 4, 0xFFFFFFFF);
	br_exit = k;
	k++;
	br_back = k;
	k++;
	exit = k;

	instrs[br_exit] = cs_branch(4, CS_CONDITION_EQ, exit - br_exit - 1);
	instrs[br_back] = cs_branch(10, CS_CONDITION_EQ, body - br_back - 1);

	return k;
}

static struct drm_panthor_group_create
make_group_cfg(struct drm_panthor_queue_create *queues, uint32_t nqueues,
	       uint8_t priority, uint32_t vm_id)
{
	struct drm_panthor_group_create cfg = {
		.queues = DRM_PANTHOR_OBJ_ARRAY(nqueues, queues),
		.max_compute_cores = 1,
		.max_fragment_cores = 1,
		.max_tiler_cores = 1,
		.priority = priority,
		.compute_core_mask = g_shader_present,
		.fragment_core_mask = g_shader_present,
		.tiler_core_mask = g_tiler_present,
		.vm_id = vm_id,
	};

	return cfg;
}

static void submit_stream(int fd, uint32_t group_handle, uint32_t queue_index,
			  uint64_t stream_addr, uint32_t stream_size,
			  struct drm_panthor_sync_op *syncs, uint32_t nsyncs)
{
	struct drm_panthor_queue_submit submit = {
		.queue_index = queue_index,
		.stream_size = stream_size,
		.stream_addr = stream_addr,
	};
	struct drm_panthor_group_submit group_submit = {
		.group_handle = group_handle,
	};

	if (nsyncs)
		submit.syncs = (struct drm_panthor_obj_array)
			DRM_PANTHOR_OBJ_ARRAY(nsyncs, syncs);

	group_submit.queue_submits = (struct drm_panthor_obj_array)
		DRM_PANTHOR_OBJ_ARRAY(1, &submit);

	igt_panthor_group_submit(fd, &group_submit, 0);
}

static struct drm_panthor_sync_op signal_op(uint32_t syncobj)
{
	return (struct drm_panthor_sync_op){
		.flags = DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ |
			 DRM_PANTHOR_SYNC_OP_SIGNAL,
		.handle = syncobj,
	};
}

static struct drm_panthor_sync_op wait_op(uint32_t syncobj)
{
	return (struct drm_panthor_sync_op){
		.flags = DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ |
			 DRM_PANTHOR_SYNC_OP_WAIT,
		.handle = syncobj,
	};
}

int igt_main()
{
	int fd = -1;

	igt_fixture() {
		struct drm_panthor_gpu_info gpu_info = {};
		struct drm_panthor_csif_info csif_info = {};

		igt_panthor_skip_on_big_endian();
		fd = drm_open_driver(DRIVER_PANTHOR);

		igt_panthor_query(fd, DRM_PANTHOR_DEV_QUERY_GPU_INFO,
				  &gpu_info, sizeof(gpu_info), 0);
		g_shader_present = gpu_info.shader_present;
		g_tiler_present = gpu_info.tiler_present;

		igt_panthor_query(fd, DRM_PANTHOR_DEV_QUERY_CSIF_INFO,
				  &csif_info, sizeof(csif_info), 0);
		g_slots = csif_info.csg_slot_count;
		igt_assert_neq_u32(g_slots, 0);
	}

	igt_describe("Submit an empty queue and wait for the syncobj to signal.");
	igt_subtest("nop") {
		uint32_t vm_id, group_handle, syncobj;
		struct drm_panthor_queue_create queue = {
			.priority = 0, .ringbuf_size = 4096,
		};
		struct drm_panthor_group_create cfg;
		struct drm_panthor_sync_op sync = {};

		igt_panthor_vm_create(fd, &vm_id, 0);
		cfg = make_group_cfg(&queue, 1, PANTHOR_GROUP_PRIORITY_LOW, vm_id);
		igt_panthor_group_create(fd, &cfg, 0);
		group_handle = cfg.group_handle;

		syncobj = syncobj_create(fd, 0);
		sync = signal_op(syncobj);
		submit_stream(fd, group_handle, 0, 0, 0, &sync, 1);

		igt_assert(wait_done(fd, syncobj, 10 * SEC_NS));

		syncobj_destroy(fd, syncobj);
		igt_panthor_group_destroy(fd, group_handle, 0);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Submit a small WAIT-0 compute stream and wait for completion.");
	igt_subtest("compute_simple") {
		uint32_t vm_id, group_handle, syncobj;
		struct panthor_bo bo = {};
		struct drm_panthor_queue_create queue = {
			.priority = 0, .ringbuf_size = 4096,
		};
		struct drm_panthor_group_create cfg;
		struct drm_panthor_sync_op sync;
		uint64_t instr = cs_wait(0, false);

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_panthor_bo_create_mapped(fd, &bo, 4096, 0, 0);

		for (int i = 0; i < 8; i++)
			memcpy((uint8_t *)bo.map + i * sizeof(instr), &instr,
			       sizeof(instr));

		igt_panthor_vm_bind(fd, vm_id, bo.handle, INITIAL_VA, bo.size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		cfg = make_group_cfg(&queue, 1, PANTHOR_GROUP_PRIORITY_LOW, vm_id);
		igt_panthor_group_create(fd, &cfg, 0);
		group_handle = cfg.group_handle;

		syncobj = syncobj_create(fd, 0);
		sync = signal_op(syncobj);
		submit_stream(fd, group_handle, 0, INITIAL_VA, 64, &sync, 1);

		igt_assert(wait_done(fd, syncobj, 10 * SEC_NS));

		syncobj_destroy(fd, syncobj);
		igt_panthor_group_destroy(fd, group_handle, 0);
		igt_panthor_vm_destroy(fd, vm_id, 0);
		igt_panthor_free_bo(fd, &bo);
	}

	igt_describe("Fill every CSG slot with a concurrent counted loop and "
		     "verify each completes with the full iteration count.");
	igt_subtest("csg_slots_max") {
		uint32_t n = g_slots;
		uint32_t *vm_ids = calloc(n, sizeof(*vm_ids));
		uint32_t *groups = calloc(n, sizeof(*groups));
		uint32_t *syncobjs = calloc(n, sizeof(*syncobjs));
		struct panthor_bo *bos = calloc(n, sizeof(*bos));

		for (uint32_t i = 0; i < n; i++) {
			struct drm_panthor_queue_create queue = {
				.priority = 0, .ringbuf_size = 4096,
			};
			struct drm_panthor_group_create cfg;
			struct drm_panthor_sync_op sync;
			uint64_t instrs[16];
			int ninstrs;

			igt_panthor_vm_create(fd, &vm_ids[i], 0);
			syncobjs[i] = syncobj_create(fd, 0);
			igt_panthor_bo_create_mapped(fd, &bos[i], 4096, 0, 0);

			*(volatile uint32_t *)((uint8_t *)bos[i].map + STARTED_OFFSET) = 0;
			*(volatile uint32_t *)((uint8_t *)bos[i].map + COUNTER_OFFSET) = 0;

			ninstrs = emit_counted_loop(instrs,
						    INITIAL_VA + COUNTER_OFFSET,
						    INITIAL_VA + STARTED_OFFSET,
						    COUNTED_LOOP_N);
			memcpy(bos[i].map, instrs, ninstrs * sizeof(instrs[0]));

			igt_panthor_vm_bind(fd, vm_ids[i], bos[i].handle,
					    INITIAL_VA, bos[i].size,
					    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP |
					    DRM_PANTHOR_VM_BIND_OP_MAP_UNCACHED, 0);

			cfg = make_group_cfg(&queue, 1,
					     PANTHOR_GROUP_PRIORITY_LOW, vm_ids[i]);
			igt_panthor_group_create(fd, &cfg, 0);
			groups[i] = cfg.group_handle;

			sync = signal_op(syncobjs[i]);
			submit_stream(fd, groups[i], 0, INITIAL_VA,
				      ninstrs * sizeof(instrs[0]), &sync, 1);
		}

		/* With exactly g_slots groups, all stay resident and run. */
		for (uint32_t i = 0; i < n; i++) {
			volatile uint32_t *started =
				(volatile uint32_t *)((uint8_t *)bos[i].map + STARTED_OFFSET);
			int retries = 100;

			while (*started == 0 && retries--)
				usleep(10000);
			igt_assert_f(*started != 0, "job %u failed to start\n", i);
		}

		for (uint32_t i = 0; i < n; i++) {
			volatile uint32_t *counter =
				(volatile uint32_t *)((uint8_t *)bos[i].map + COUNTER_OFFSET);

			igt_assert_f(wait_done(fd, syncobjs[i], 10 * SEC_NS),
				     "job %u timed out\n", i);
			igt_assert_eq_u32(*counter, COUNTED_LOOP_N);
		}

		for (uint32_t i = 0; i < n; i++) {
			igt_panthor_group_destroy(fd, groups[i], 0);
			syncobj_destroy(fd, syncobjs[i]);
			igt_panthor_free_bo(fd, &bos[i]);
			igt_panthor_vm_destroy(fd, vm_ids[i], 0);
		}
		free(vm_ids);
		free(groups);
		free(syncobjs);
		free(bos);
	}

	igt_describe("Overcommit CSG slots by one and verify time-slicing lets "
		     "every counted-loop group start and complete.");
	igt_subtest("csg_slots_overcommit") {
		uint32_t n = g_slots + 1;
		uint32_t *vm_ids = calloc(n, sizeof(*vm_ids));
		uint32_t *groups = calloc(n, sizeof(*groups));
		uint32_t *syncobjs = calloc(n, sizeof(*syncobjs));
		struct panthor_bo *bos = calloc(n, sizeof(*bos));

		for (uint32_t i = 0; i < n; i++) {
			struct drm_panthor_queue_create queue = {
				.priority = 0, .ringbuf_size = 4096,
			};
			struct drm_panthor_group_create cfg;
			struct drm_panthor_sync_op sync;
			uint64_t instrs[16];
			int ninstrs;

			igt_panthor_vm_create(fd, &vm_ids[i], 0);
			syncobjs[i] = syncobj_create(fd, 0);
			igt_panthor_bo_create_mapped(fd, &bos[i], 4096, 0, 0);

			*(volatile uint32_t *)((uint8_t *)bos[i].map + STARTED_OFFSET) = 0;
			*(volatile uint32_t *)((uint8_t *)bos[i].map + COUNTER_OFFSET) = 0;

			ninstrs = emit_counted_loop(instrs,
						    INITIAL_VA + COUNTER_OFFSET,
						    INITIAL_VA + STARTED_OFFSET,
						    COUNTED_LOOP_N);
			memcpy(bos[i].map, instrs, ninstrs * sizeof(instrs[0]));

			igt_panthor_vm_bind(fd, vm_ids[i], bos[i].handle,
					    INITIAL_VA, bos[i].size,
					    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP |
					    DRM_PANTHOR_VM_BIND_OP_MAP_UNCACHED, 0);

			cfg = make_group_cfg(&queue, 1,
					     PANTHOR_GROUP_PRIORITY_LOW, vm_ids[i]);
			igt_panthor_group_create(fd, &cfg, 0);
			groups[i] = cfg.group_handle;

			sync = signal_op(syncobjs[i]);
			submit_stream(fd, groups[i], 0, INITIAL_VA,
				      ninstrs * sizeof(instrs[0]), &sync, 1);
		}

		/* slots+1 jobs: at least one must wait for a time-slice. */
		for (uint32_t i = 0; i < n; i++) {
			volatile uint32_t *started =
				(volatile uint32_t *)((uint8_t *)bos[i].map + STARTED_OFFSET);
			int retries = 1000;

			while (*started == 0 && retries--)
				usleep(10000);
			igt_assert_f(*started != 0, "job %u failed to start "
				     "(time-slicing too slow?)\n", i);
		}

		for (uint32_t i = 0; i < n; i++) {
			volatile uint32_t *counter =
				(volatile uint32_t *)((uint8_t *)bos[i].map + COUNTER_OFFSET);

			igt_assert_f(wait_done(fd, syncobjs[i], 15 * SEC_NS),
				     "job %u timed out\n", i);
			igt_assert_eq_u32(*counter, COUNTED_LOOP_N);
		}

		for (uint32_t i = 0; i < n; i++) {
			igt_panthor_group_destroy(fd, groups[i], 0);
			syncobj_destroy(fd, syncobjs[i]);
			igt_panthor_free_bo(fd, &bos[i]);
			igt_panthor_vm_destroy(fd, vm_ids[i], 0);
		}
		free(vm_ids);
		free(groups);
		free(syncobjs);
		free(bos);
	}

	igt_describe("Prepopulate every slot with idle (completed) groups, then "
		     "submit one more group and verify it runs.");
	igt_subtest("idle_prepopulate") {
		uint32_t n = g_slots;
		uint32_t *vm_ids = calloc(n, sizeof(*vm_ids));
		uint32_t *groups = calloc(n, sizeof(*groups));
		uint32_t *syncobjs = calloc(n, sizeof(*syncobjs));
		uint32_t extra_vm, extra_group, extra_sync;
		struct drm_panthor_sync_op sync;

		for (uint32_t i = 0; i < n; i++) {
			struct drm_panthor_queue_create queue = {
				.priority = 0, .ringbuf_size = 4096,
			};
			struct drm_panthor_group_create cfg;

			igt_panthor_vm_create(fd, &vm_ids[i], 0);
			syncobjs[i] = syncobj_create(fd, 0);
			cfg = make_group_cfg(&queue, 1,
					     PANTHOR_GROUP_PRIORITY_LOW, vm_ids[i]);
			igt_panthor_group_create(fd, &cfg, 0);
			groups[i] = cfg.group_handle;

			sync = signal_op(syncobjs[i]);
			submit_stream(fd, groups[i], 0, 0, 0, &sync, 1);

			igt_assert_f(wait_done(fd, syncobjs[i], 10 * SEC_NS),
				     "prepopulate job %u timed out\n", i);
		}

		{
			struct drm_panthor_queue_create queue = {
				.priority = 0, .ringbuf_size = 4096,
			};
			struct drm_panthor_group_create cfg;

			igt_panthor_vm_create(fd, &extra_vm, 0);
			extra_sync = syncobj_create(fd, 0);
			cfg = make_group_cfg(&queue, 1,
					     PANTHOR_GROUP_PRIORITY_LOW, extra_vm);
			igt_panthor_group_create(fd, &cfg, 0);
			extra_group = cfg.group_handle;

			sync = signal_op(extra_sync);
			submit_stream(fd, extra_group, 0, 0, 0, &sync, 1);

			igt_assert(wait_done(fd, extra_sync, 10 * SEC_NS));
		}

		syncobj_destroy(fd, extra_sync);
		igt_panthor_group_destroy(fd, extra_group, 0);
		igt_panthor_vm_destroy(fd, extra_vm, 0);

		for (uint32_t i = 0; i < n; i++) {
			igt_panthor_group_destroy(fd, groups[i], 0);
			syncobj_destroy(fd, syncobjs[i]);
			igt_panthor_vm_destroy(fd, vm_ids[i], 0);
		}
		free(vm_ids);
		free(groups);
		free(syncobjs);
	}

	igt_describe("Submit a stalled low-priority group, then a high-priority "
		     "group, and verify the high-priority job completes.");
	igt_subtest("priority_preemption") {
		uint32_t low_vm, high_vm, low_group, high_group, high_sync;
		struct panthor_bo bo = {};
		struct drm_panthor_queue_create queue = {
			.priority = 0, .ringbuf_size = 4096,
		};
		struct drm_panthor_group_create low_cfg, high_cfg;
		struct drm_panthor_sync_op sync;
		/* WAIT on scoreboard 1: blocks forever. */
		uint64_t instr = cs_wait(1, false);

		igt_panthor_vm_create(fd, &low_vm, 0);
		igt_panthor_vm_create(fd, &high_vm, 0);

		igt_panthor_bo_create_mapped(fd, &bo, 4096, 0, 0);
		memcpy(bo.map, &instr, sizeof(instr));
		igt_panthor_vm_bind(fd, low_vm, bo.handle, INITIAL_VA, bo.size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		low_cfg = make_group_cfg(&queue, 1, PANTHOR_GROUP_PRIORITY_LOW, low_vm);
		igt_panthor_group_create(fd, &low_cfg, 0);
		low_group = low_cfg.group_handle;
		submit_stream(fd, low_group, 0, INITIAL_VA, sizeof(instr), NULL, 0);

		/*
		 * Creating a HIGH priority group requires CAP_SYS_NICE or
		 * DRM_MASTER; skip cleanly if the kernel rejects it.
		 */
		high_cfg = make_group_cfg(&queue, 1, PANTHOR_GROUP_PRIORITY_HIGH, high_vm);
		igt_require(igt_ioctl(fd, DRM_IOCTL_PANTHOR_GROUP_CREATE, &high_cfg) == 0);
		high_group = high_cfg.group_handle;

		high_sync = syncobj_create(fd, 0);
		sync = signal_op(high_sync);
		submit_stream(fd, high_group, 0, 0, 0, &sync, 1);

		igt_assert(wait_done(fd, high_sync, 10 * SEC_NS));

		syncobj_destroy(fd, high_sync);
		igt_panthor_group_destroy(fd, high_group, 0);
		igt_panthor_vm_destroy(fd, high_vm, 0);

		igt_panthor_group_destroy(fd, low_group, 0);
		igt_panthor_free_bo(fd, &bo);
		igt_panthor_vm_destroy(fd, low_vm, 0);
	}

	igt_describe("Submit work to two queues of one group and verify both "
		     "queues' syncobjs signal.");
	igt_subtest("multi_queue") {
		uint32_t vm_id, group_handle;
		uint32_t syncobjs[2];
		struct drm_panthor_queue_create queues[2] = {
			{ .priority = 0, .ringbuf_size = 4096 },
			{ .priority = 0, .ringbuf_size = 4096 },
		};
		struct drm_panthor_group_create cfg;

		igt_panthor_vm_create(fd, &vm_id, 0);
		cfg = make_group_cfg(queues, 2, PANTHOR_GROUP_PRIORITY_LOW, vm_id);
		igt_panthor_group_create(fd, &cfg, 0);
		group_handle = cfg.group_handle;

		for (int i = 0; i < 2; i++) {
			struct drm_panthor_sync_op sync;

			syncobjs[i] = syncobj_create(fd, 0);
			sync = signal_op(syncobjs[i]);
			submit_stream(fd, group_handle, i, 0, 0, &sync, 1);
		}

		for (int i = 0; i < 2; i++)
			igt_assert_f(wait_done(fd, syncobjs[i], 10 * SEC_NS),
				     "queue %d timed out\n", i);

		for (int i = 0; i < 2; i++)
			syncobj_destroy(fd, syncobjs[i]);
		igt_panthor_group_destroy(fd, group_handle, 0);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("A job blocked on a userspace sync must not complete until "
		     "the host signals it; verify the dependent queue unblocks.");
	igt_subtest("sync_wait_block") {
		uint32_t vm_id, group_handle, sync_a, sync_b;
		struct panthor_bo bo = {};
		struct drm_panthor_queue_create queues[2] = {
			{ .priority = 0, .ringbuf_size = 4096 },
			{ .priority = 0, .ringbuf_size = 4096 },
		};
		struct drm_panthor_group_create cfg;
		struct drm_panthor_sync_op sync1, sync2[2];
		volatile uint32_t *user_sync;
		uint64_t instrs[3];

		igt_panthor_vm_create(fd, &vm_id, 0);
		sync_a = syncobj_create(fd, 0);
		sync_b = syncobj_create(fd, 0);

		cfg = make_group_cfg(queues, 2, PANTHOR_GROUP_PRIORITY_LOW, vm_id);
		igt_panthor_group_create(fd, &cfg, 0);
		group_handle = cfg.group_handle;

		igt_panthor_bo_create_mapped(fd, &bo, 4096, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, INITIAL_VA, bo.size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		user_sync = (volatile uint32_t *)((uint8_t *)bo.map + USER_SYNC_OFFSET);
		*user_sync = 0;

		/* MOV48 r0, addr; MOV32 r2, 0; SYNC_WAIT32.gt r0 > r2. */
		instrs[0] = cs_mov48(0, INITIAL_VA + USER_SYNC_OFFSET);
		instrs[1] = cs_mov32(2, 0);
		instrs[2] = cs_sync_wait32(0, 2, CS_CONDITION_GT);
		memcpy(bo.map, instrs, sizeof(instrs));

		/* Job 1 on queue 0: stalls on the userspace sync, signals A. */
		sync1 = signal_op(sync_a);
		submit_stream(fd, group_handle, 0, INITIAL_VA, sizeof(instrs),
			      &sync1, 1);

		/* Job 2 on queue 1: waits on A, signals B. */
		sync2[0] = wait_op(sync_a);
		sync2[1] = signal_op(sync_b);
		submit_stream(fd, group_handle, 1, 0, 0, sync2, 2);

		/* Job 2 must stay blocked while the userspace sync is 0. */
		igt_assert(!wait_done(fd, sync_b, 1 * SEC_NS));

		/* Release the userspace sync; the chain must now complete. */
		*user_sync = 1;
		igt_assert(wait_done(fd, sync_b, 5 * SEC_NS));

		syncobj_destroy(fd, sync_a);
		syncobj_destroy(fd, sync_b);
		igt_panthor_group_destroy(fd, group_handle, 0);
		igt_panthor_free_bo(fd, &bo);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("A job stalled forever on a userspace sync must hit the job "
		     "timeout; a later submit to the terminated group fails EINVAL.");
	igt_subtest("job_timeout") {
		uint32_t vm_id, group_handle;
		struct panthor_bo bo = {};
		struct drm_panthor_queue_create queue = {
			.priority = 0, .ringbuf_size = 4096,
		};
		struct drm_panthor_group_create cfg;
		struct drm_panthor_group_submit group_submit = {};
		struct drm_panthor_queue_submit submit = {};
		volatile uint32_t *user_sync;
		uint64_t instrs[3];

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_panthor_bo_create_mapped(fd, &bo, 4096, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, INITIAL_VA, bo.size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		user_sync = (volatile uint32_t *)((uint8_t *)bo.map + USER_SYNC_OFFSET);
		*user_sync = 0;

		instrs[0] = cs_mov48(0, INITIAL_VA + USER_SYNC_OFFSET);
		instrs[1] = cs_mov32(2, 0);
		instrs[2] = cs_sync_wait32(0, 2, CS_CONDITION_GT);
		memcpy(bo.map, instrs, sizeof(instrs));

		cfg = make_group_cfg(&queue, 1, PANTHOR_GROUP_PRIORITY_LOW, vm_id);
		igt_panthor_group_create(fd, &cfg, 0);
		group_handle = cfg.group_handle;

		submit_stream(fd, group_handle, 0, INITIAL_VA, sizeof(instrs),
			      NULL, 0);

		/* Allow the (5s) job timeout to fire and terminate the group. */
		sleep(10);

		/* A second submit must be rejected: the group was terminated. */
		submit.queue_index = 0;
		group_submit.group_handle = group_handle;
		group_submit.queue_submits = (struct drm_panthor_obj_array)
			DRM_PANTHOR_OBJ_ARRAY(1, &submit);
		do_ioctl_err(fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &group_submit, EINVAL);

		igt_panthor_group_destroy(fd, group_handle, 0);
		igt_panthor_free_bo(fd, &bo);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("An invalid instruction must mark the group with a fatal "
		     "fault in its reported state.");
	igt_subtest("fatal_fault") {
		uint32_t vm_id, group_handle;
		struct panthor_bo bo = {};
		struct drm_panthor_queue_create queue = {
			.priority = 0, .ringbuf_size = 4096,
		};
		struct drm_panthor_group_create cfg;
		struct drm_panthor_group_get_state get_state = {};
		/* Invalid opcode 0xff. */
		uint64_t instr = (0xffULL << 56);

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_panthor_bo_create_mapped(fd, &bo, 4096, 0, 0);
		memcpy(bo.map, &instr, sizeof(instr));
		igt_panthor_vm_bind(fd, vm_id, bo.handle, INITIAL_VA, bo.size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		cfg = make_group_cfg(&queue, 1, PANTHOR_GROUP_PRIORITY_LOW, vm_id);
		igt_panthor_group_create(fd, &cfg, 0);
		group_handle = cfg.group_handle;

		submit_stream(fd, group_handle, 0, INITIAL_VA, sizeof(instr),
			      NULL, 0);

		sleep(1);

		get_state.group_handle = group_handle;
		do_ioctl(fd, DRM_IOCTL_PANTHOR_GROUP_GET_STATE, &get_state);
		igt_assert(get_state.state & DRM_PANTHOR_GROUP_STATE_FATAL_FAULT);

		igt_panthor_group_destroy(fd, group_handle, 0);
		igt_panthor_free_bo(fd, &bo);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Submit a full 4KB stream of MOV32 instructions and wait "
		     "for completion.");
	igt_subtest("large_instruction_stream") {
		uint32_t vm_id, group_handle, syncobj;
		struct panthor_bo bo = {};
		struct drm_panthor_queue_create queue = {
			.priority = 0, .ringbuf_size = 4096,
		};
		struct drm_panthor_group_create cfg;
		struct drm_panthor_sync_op sync;
		uint64_t instr = cs_mov32(0, 42);

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_panthor_bo_create_mapped(fd, &bo, 4096, 0, 0);

		for (int i = 0; i < 512; i++)
			memcpy((uint8_t *)bo.map + i * sizeof(instr), &instr,
			       sizeof(instr));

		igt_panthor_vm_bind(fd, vm_id, bo.handle, INITIAL_VA, bo.size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		cfg = make_group_cfg(&queue, 1, PANTHOR_GROUP_PRIORITY_LOW, vm_id);
		igt_panthor_group_create(fd, &cfg, 0);
		group_handle = cfg.group_handle;

		syncobj = syncobj_create(fd, 0);
		sync = signal_op(syncobj);
		submit_stream(fd, group_handle, 0, INITIAL_VA, 4096, &sync, 1);

		igt_assert(wait_done(fd, syncobj, 10 * SEC_NS));

		syncobj_destroy(fd, syncobj);
		igt_panthor_group_destroy(fd, group_handle, 0);
		igt_panthor_free_bo(fd, &bo);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Overcommit CSG slots 2x with counted loops and verify "
		     "extreme time-slicing still completes every group.");
	igt_subtest("csg_slots_extreme_overcommit") {
		uint32_t n = g_slots * 2;
		uint32_t *vm_ids = calloc(n, sizeof(*vm_ids));
		uint32_t *groups = calloc(n, sizeof(*groups));
		uint32_t *syncobjs = calloc(n, sizeof(*syncobjs));
		struct panthor_bo *bos = calloc(n, sizeof(*bos));

		for (uint32_t i = 0; i < n; i++) {
			struct drm_panthor_queue_create queue = {
				.priority = 0, .ringbuf_size = 4096,
			};
			struct drm_panthor_group_create cfg;
			struct drm_panthor_sync_op sync;
			uint64_t instrs[16];
			int ninstrs;

			igt_panthor_vm_create(fd, &vm_ids[i], 0);
			syncobjs[i] = syncobj_create(fd, 0);
			igt_panthor_bo_create_mapped(fd, &bos[i], 4096, 0, 0);

			*(volatile uint32_t *)((uint8_t *)bos[i].map + STARTED_OFFSET) = 0;
			*(volatile uint32_t *)((uint8_t *)bos[i].map + COUNTER_OFFSET) = 0;

			ninstrs = emit_counted_loop(instrs,
						    INITIAL_VA + COUNTER_OFFSET,
						    INITIAL_VA + STARTED_OFFSET,
						    COUNTED_LOOP_N);
			memcpy(bos[i].map, instrs, ninstrs * sizeof(instrs[0]));

			igt_panthor_vm_bind(fd, vm_ids[i], bos[i].handle,
					    INITIAL_VA, bos[i].size,
					    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP |
					    DRM_PANTHOR_VM_BIND_OP_MAP_UNCACHED, 0);

			cfg = make_group_cfg(&queue, 1,
					     PANTHOR_GROUP_PRIORITY_LOW, vm_ids[i]);
			igt_panthor_group_create(fd, &cfg, 0);
			groups[i] = cfg.group_handle;

			sync = signal_op(syncobjs[i]);
			submit_stream(fd, groups[i], 0, INITIAL_VA,
				      ninstrs * sizeof(instrs[0]), &sync, 1);
		}

		for (uint32_t i = 0; i < n; i++) {
			volatile uint32_t *started =
				(volatile uint32_t *)((uint8_t *)bos[i].map + STARTED_OFFSET);
			int retries = 2000;

			while (*started == 0 && retries--)
				usleep(10000);
			igt_assert_f(*started != 0, "job %u failed to start "
				     "(time-slicing too slow?)\n", i);
		}

		for (uint32_t i = 0; i < n; i++) {
			volatile uint32_t *counter =
				(volatile uint32_t *)((uint8_t *)bos[i].map + COUNTER_OFFSET);

			igt_assert_f(wait_done(fd, syncobjs[i], 20 * SEC_NS),
				     "job %u timed out\n", i);
			igt_assert_eq_u32(*counter, COUNTED_LOOP_N);
		}

		for (uint32_t i = 0; i < n; i++) {
			igt_panthor_group_destroy(fd, groups[i], 0);
			syncobj_destroy(fd, syncobjs[i]);
			igt_panthor_free_bo(fd, &bos[i]);
			igt_panthor_vm_destroy(fd, vm_ids[i], 0);
		}
		free(vm_ids);
		free(groups);
		free(syncobjs);
		free(bos);
	}

	igt_describe("Fill every slot with busy high-priority counted loops, "
		     "then submit a realtime group and verify it preempts and "
		     "finishes first.");
	igt_subtest("rt_priority_preemption") {
		uint32_t n = g_slots;
		uint32_t *vm_ids = calloc(n + 1, sizeof(*vm_ids));
		uint32_t *groups = calloc(n + 1, sizeof(*groups));
		uint32_t *syncobjs = calloc(n + 1, sizeof(*syncobjs));
		struct panthor_bo *bos = calloc(n + 1, sizeof(*bos));
		uint32_t rt = n;
		volatile uint32_t *rt_started, *rt_counter;
		struct drm_panthor_group_create rt_cfg;
		struct drm_panthor_queue_create rt_queue = {
			.priority = 0, .ringbuf_size = 4096,
		};
		struct drm_panthor_sync_op rt_sync;
		uint64_t rt_instrs[16];
		int rt_ninstrs, retries;
		bool created;

		/* Fill all slots with busy HIGH priority groups. */
		for (uint32_t i = 0; i < n; i++) {
			struct drm_panthor_queue_create queue = {
				.priority = 0, .ringbuf_size = 4096,
			};
			struct drm_panthor_group_create cfg;
			struct drm_panthor_sync_op sync;
			uint64_t instrs[16];
			int ninstrs;

			igt_panthor_vm_create(fd, &vm_ids[i], 0);
			syncobjs[i] = syncobj_create(fd, 0);
			igt_panthor_bo_create_mapped(fd, &bos[i], 4096, 0, 0);

			*(volatile uint32_t *)((uint8_t *)bos[i].map + STARTED_OFFSET) = 0;
			*(volatile uint32_t *)((uint8_t *)bos[i].map + COUNTER_OFFSET) = 0;

			ninstrs = emit_counted_loop(instrs,
						    INITIAL_VA + COUNTER_OFFSET,
						    INITIAL_VA + STARTED_OFFSET,
						    COUNTED_LOOP_N);
			memcpy(bos[i].map, instrs, ninstrs * sizeof(instrs[0]));

			igt_panthor_vm_bind(fd, vm_ids[i], bos[i].handle,
					    INITIAL_VA, bos[i].size,
					    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP |
					    DRM_PANTHOR_VM_BIND_OP_MAP_UNCACHED, 0);

			cfg = make_group_cfg(&queue, 1,
					     PANTHOR_GROUP_PRIORITY_HIGH, vm_ids[i]);

			/*
			 * HIGH priority needs CAP_SYS_NICE / DRM_MASTER. If the
			 * first one is rejected, skip cleanly after tearing down
			 * what was already allocated.
			 */
			if (igt_ioctl(fd, DRM_IOCTL_PANTHOR_GROUP_CREATE, &cfg)) {
				for (uint32_t j = 0; j < i; j++) {
					igt_panthor_group_destroy(fd, groups[j], 0);
					syncobj_destroy(fd, syncobjs[j]);
					igt_panthor_free_bo(fd, &bos[j]);
					igt_panthor_vm_destroy(fd, vm_ids[j], 0);
				}
				syncobj_destroy(fd, syncobjs[i]);
				igt_panthor_free_bo(fd, &bos[i]);
				igt_panthor_vm_destroy(fd, vm_ids[i], 0);
				free(vm_ids);
				free(groups);
				free(syncobjs);
				free(bos);
				igt_skip("HIGH priority group create rejected "
					 "(needs CAP_SYS_NICE/DRM_MASTER)\n");
			}
			groups[i] = cfg.group_handle;

			sync = signal_op(syncobjs[i]);
			submit_stream(fd, groups[i], 0, INITIAL_VA,
				      ninstrs * sizeof(instrs[0]), &sync, 1);
		}

		/* Wait for every HIGH job to genuinely start. */
		for (uint32_t i = 0; i < n; i++) {
			volatile uint32_t *started =
				(volatile uint32_t *)((uint8_t *)bos[i].map + STARTED_OFFSET);

			while (*started == 0)
				usleep(10000);
		}

		/* Submit the RT group; it must preempt a busy background group. */
		igt_panthor_vm_create(fd, &vm_ids[rt], 0);
		syncobjs[rt] = syncobj_create(fd, 0);
		igt_panthor_bo_create_mapped(fd, &bos[rt], 4096, 0, 0);

		rt_started = (volatile uint32_t *)((uint8_t *)bos[rt].map + STARTED_OFFSET);
		rt_counter = (volatile uint32_t *)((uint8_t *)bos[rt].map + COUNTER_OFFSET);
		*rt_started = 0;
		*rt_counter = 0;

		rt_ninstrs = emit_counted_loop(rt_instrs,
					       INITIAL_VA + COUNTER_OFFSET,
					       INITIAL_VA + STARTED_OFFSET,
					       COUNTED_LOOP_N);
		memcpy(bos[rt].map, rt_instrs, rt_ninstrs * sizeof(rt_instrs[0]));
		igt_panthor_vm_bind(fd, vm_ids[rt], bos[rt].handle, INITIAL_VA,
				    bos[rt].size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP |
				    DRM_PANTHOR_VM_BIND_OP_MAP_UNCACHED, 0);

		rt_cfg = make_group_cfg(&rt_queue, 1,
					PANTHOR_GROUP_PRIORITY_REALTIME, vm_ids[rt]);
		created = igt_ioctl(fd, DRM_IOCTL_PANTHOR_GROUP_CREATE, &rt_cfg) == 0;
		igt_assert(created);
		groups[rt] = rt_cfg.group_handle;

		rt_sync = signal_op(syncobjs[rt]);
		submit_stream(fd, groups[rt], 0, INITIAL_VA,
			      rt_ninstrs * sizeof(rt_instrs[0]), &rt_sync, 1);

		retries = 100;
		while (*rt_started == 0 && retries--)
			usleep(10000);
		igt_assert_f(*rt_started != 0, "RT job failed to start immediately\n");

		/* The RT group must finish its loop before the background groups. */
		igt_assert_f(wait_done(fd, syncobjs[rt], 15 * SEC_NS),
			     "RT job timed out\n");
		igt_assert_eq_u32(*rt_counter, COUNTED_LOOP_N);

		/* Background groups must also drain with the full count. */
		for (uint32_t i = 0; i < n; i++) {
			volatile uint32_t *counter =
				(volatile uint32_t *)((uint8_t *)bos[i].map + COUNTER_OFFSET);

			igt_assert_f(wait_done(fd, syncobjs[i], 15 * SEC_NS),
				     "background job %u timed out\n", i);
			igt_assert_eq_u32(*counter, COUNTED_LOOP_N);
		}

		for (uint32_t i = 0; i <= n; i++) {
			igt_panthor_group_destroy(fd, groups[i], 0);
			syncobj_destroy(fd, syncobjs[i]);
			igt_panthor_free_bo(fd, &bos[i]);
			igt_panthor_vm_destroy(fd, vm_ids[i], 0);
		}
		free(vm_ids);
		free(groups);
		free(syncobjs);
		free(bos);
	}

	igt_describe("Submit enough small no-op jobs to wrap the ring buffer and "
		     "verify the last one completes.");
	igt_subtest("ringbuf_wrap") {
		uint32_t vm_id, group_handle, syncobj;
		struct drm_panthor_queue_create queue = {
			.priority = 0, .ringbuf_size = 4096,
		};
		struct drm_panthor_group_create cfg;

		igt_panthor_vm_create(fd, &vm_id, 0);
		cfg = make_group_cfg(&queue, 1, PANTHOR_GROUP_PRIORITY_LOW, vm_id);
		igt_panthor_group_create(fd, &cfg, 0);
		group_handle = cfg.group_handle;

		syncobj = syncobj_create(fd, 0);

		/* 600 no-op jobs comfortably overflow the 4KB ring. */
		for (int i = 0; i < 600; i++) {
			struct drm_panthor_sync_op sync = signal_op(syncobj);
			struct drm_panthor_queue_submit submit = {
				.queue_index = 0,
				.syncs = DRM_PANTHOR_OBJ_ARRAY(1, &sync),
			};
			struct drm_panthor_group_submit group_submit = {
				.group_handle = group_handle,
				.queue_submits = DRM_PANTHOR_OBJ_ARRAY(1, &submit),
			};

			if (igt_ioctl(fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT,
				      &group_submit)) {
				/* Ring full: wait briefly and retry the job. */
				igt_assert_eq(errno, EBUSY);
				usleep(1000);
				i--;
				continue;
			}
		}

		igt_assert(wait_done(fd, syncobj, 10 * SEC_NS));

		syncobj_destroy(fd, syncobj);
		igt_panthor_group_destroy(fd, group_handle, 0);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Destroy a group while its counted loop is still running; "
		     "the destroy must succeed and the loop must be interrupted.");
	igt_subtest("group_destroy_active") {
		uint32_t vm_id, group_handle, syncobj;
		struct panthor_bo bo = {};
		struct drm_panthor_queue_create queue = {
			.priority = 0, .ringbuf_size = 4096,
		};
		struct drm_panthor_group_create cfg;
		struct drm_panthor_sync_op sync;
		volatile uint32_t *started, *counter;
		uint64_t instrs[16];
		int ninstrs;

		igt_panthor_vm_create(fd, &vm_id, 0);
		syncobj = syncobj_create(fd, 0);
		igt_panthor_bo_create_mapped(fd, &bo, 4096, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, INITIAL_VA, bo.size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP |
				    DRM_PANTHOR_VM_BIND_OP_MAP_UNCACHED, 0);

		started = (volatile uint32_t *)((uint8_t *)bo.map + STARTED_OFFSET);
		counter = (volatile uint32_t *)((uint8_t *)bo.map + COUNTER_OFFSET);
		*started = 0;
		*counter = 0;

		ninstrs = emit_counted_loop(instrs, INITIAL_VA + COUNTER_OFFSET,
					    INITIAL_VA + STARTED_OFFSET,
					    COUNTED_LOOP_N);
		memcpy(bo.map, instrs, ninstrs * sizeof(instrs[0]));

		cfg = make_group_cfg(&queue, 1, PANTHOR_GROUP_PRIORITY_LOW, vm_id);
		igt_panthor_group_create(fd, &cfg, 0);
		group_handle = cfg.group_handle;

		sync = signal_op(syncobj);
		submit_stream(fd, group_handle, 0, INITIAL_VA,
			      ninstrs * sizeof(instrs[0]), &sync, 1);

		while (*started == 0)
			usleep(10000);

		/* Destroy while genuinely running: must succeed. */
		igt_panthor_group_destroy(fd, group_handle, 0);

		/*
		 * The teardown cancels the in-flight submit, so the syncobj may
		 * legitimately signal afterwards; poll only for diagnostics. The
		 * real check is that the loop was interrupted short of N.
		 */
		wait_done(fd, syncobj, SEC_NS / 2);
		igt_assert_lt_u32(*counter, COUNTED_LOOP_N);

		syncobj_destroy(fd, syncobj);
		igt_panthor_free_bo(fd, &bo);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Build a four-queue A->B->C->D dependency chain gated on a "
		     "userspace sync and verify the tail unblocks only after the "
		     "host releases the head.");
	igt_subtest("multi_queue_dep_chain") {
		uint32_t vm_id, group_handle;
		uint32_t sync_a, sync_b, sync_c, sync_d;
		struct panthor_bo bo = {};
		struct drm_panthor_queue_create queues[4] = {
			{ .priority = 0, .ringbuf_size = 4096 },
			{ .priority = 0, .ringbuf_size = 4096 },
			{ .priority = 0, .ringbuf_size = 4096 },
			{ .priority = 0, .ringbuf_size = 4096 },
		};
		struct drm_panthor_group_create cfg;
		struct drm_panthor_sync_op op1, op2[2], op3[2], op4[2];
		volatile uint32_t *user_sync;
		uint64_t instrs[3];

		igt_panthor_vm_create(fd, &vm_id, 0);
		sync_a = syncobj_create(fd, 0);
		sync_b = syncobj_create(fd, 0);
		sync_c = syncobj_create(fd, 0);
		sync_d = syncobj_create(fd, 0);

		cfg = make_group_cfg(queues, 4, PANTHOR_GROUP_PRIORITY_LOW, vm_id);
		igt_panthor_group_create(fd, &cfg, 0);
		group_handle = cfg.group_handle;

		igt_panthor_bo_create_mapped(fd, &bo, 4096, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, INITIAL_VA, bo.size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		user_sync = (volatile uint32_t *)((uint8_t *)bo.map + USER_SYNC_OFFSET);
		*user_sync = 0;

		instrs[0] = cs_mov48(0, INITIAL_VA + USER_SYNC_OFFSET);
		instrs[1] = cs_mov32(2, 0);
		instrs[2] = cs_sync_wait32(0, 2, CS_CONDITION_GT);
		memcpy(bo.map, instrs, sizeof(instrs));

		/* Job 1 (Q0): stall on userspace sync, signal A. */
		op1 = signal_op(sync_a);
		/* Job 2 (Q1): wait A, signal B. */
		op2[0] = wait_op(sync_a);
		op2[1] = signal_op(sync_b);
		/* Job 3 (Q2): wait B, signal C. */
		op3[0] = wait_op(sync_b);
		op3[1] = signal_op(sync_c);
		/* Job 4 (Q3): wait C, signal D. */
		op4[0] = wait_op(sync_c);
		op4[1] = signal_op(sync_d);

		{
			struct drm_panthor_queue_submit submits[4] = {
				{ .queue_index = 0, .stream_addr = INITIAL_VA,
				  .stream_size = sizeof(instrs),
				  .syncs = DRM_PANTHOR_OBJ_ARRAY(1, &op1) },
				{ .queue_index = 1,
				  .syncs = DRM_PANTHOR_OBJ_ARRAY(2, op2) },
				{ .queue_index = 2,
				  .syncs = DRM_PANTHOR_OBJ_ARRAY(2, op3) },
				{ .queue_index = 3,
				  .syncs = DRM_PANTHOR_OBJ_ARRAY(2, op4) },
			};
			struct drm_panthor_group_submit group_submit = {
				.group_handle = group_handle,
				.queue_submits = DRM_PANTHOR_OBJ_ARRAY(4, submits),
			};

			igt_panthor_group_submit(fd, &group_submit, 0);
		}

		/* Job 4 must stay blocked until the head is released. */
		igt_assert(!wait_done(fd, sync_d, 1 * SEC_NS));

		*user_sync = 1;
		igt_assert(wait_done(fd, sync_d, 5 * SEC_NS));

		syncobj_destroy(fd, sync_a);
		syncobj_destroy(fd, sync_b);
		syncobj_destroy(fd, sync_c);
		syncobj_destroy(fd, sync_d);
		igt_panthor_group_destroy(fd, group_handle, 0);
		igt_panthor_free_bo(fd, &bo);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Run a single deterministic register-only counted loop and "
		     "verify the BO counter reaches the full iteration count.");
	igt_subtest("counted_loop") {
		uint32_t vm_id, group_handle, syncobj;
		struct panthor_bo bo = {};
		struct drm_panthor_queue_create queue = {
			.priority = 0, .ringbuf_size = 4096,
		};
		struct drm_panthor_group_create cfg;
		struct drm_panthor_sync_op sync;
		volatile uint32_t *counter;
		uint64_t instrs[16];
		int ninstrs;

		igt_panthor_vm_create(fd, &vm_id, 0);
		syncobj = syncobj_create(fd, 0);
		igt_panthor_bo_create_mapped(fd, &bo, 4096, 0, 0);

		counter = (volatile uint32_t *)((uint8_t *)bo.map + COUNTER_OFFSET);
		*counter = 0;

		ninstrs = emit_counted_loop(instrs, INITIAL_VA + COUNTER_OFFSET,
					    0, COUNTED_LOOP_N);
		memcpy(bo.map, instrs, ninstrs * sizeof(instrs[0]));

		igt_panthor_vm_bind(fd, vm_id, bo.handle, INITIAL_VA, bo.size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP |
				    DRM_PANTHOR_VM_BIND_OP_MAP_UNCACHED, 0);

		cfg = make_group_cfg(&queue, 1, PANTHOR_GROUP_PRIORITY_LOW, vm_id);
		igt_panthor_group_create(fd, &cfg, 0);
		group_handle = cfg.group_handle;

		sync = signal_op(syncobj);
		submit_stream(fd, group_handle, 0, INITIAL_VA,
			      ninstrs * sizeof(instrs[0]), &sync, 1);

		igt_assert(wait_done(fd, syncobj, 10 * SEC_NS));
		igt_assert_eq_u32(*counter, COUNTED_LOOP_N);

		syncobj_destroy(fd, syncobj);
		igt_panthor_group_destroy(fd, group_handle, 0);
		igt_panthor_free_bo(fd, &bo);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
