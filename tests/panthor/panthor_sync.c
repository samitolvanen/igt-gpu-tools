// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (C) 2026 Google LLC

#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "igt.h"
#include "igt_panthor.h"
#include "igt_syncobj.h"
#include "panthor_drm.h"
#include "sw_sync.h"

/*
 * Sync-operation tests for the Tyr/Panthor driver.
 *
 * A submission ioctl takes an array of jobs (GROUP_SUBMIT) or of bind
 * operations (VM_BIND_ASYNC), and every entry carries its own sync
 * operations. A wait must resolve against the signals of its own batch, so an
 * entry can wait on a syncobj an earlier entry signals. When the only signal
 * in the batch comes from a later entry, the wait resolves against the fence
 * the syncobj carried before the ioctl.
 */

#define PAGE_SIZE	4096ULL

/* Page-aligned GPU VAs inside the user range, unlikely to collide. */
#define VA_A		0x100000ULL
#define VA_B		0x101000ULL
#define VA_C		0x102000ULL

/* Byte offset of the word the host writes to release a blocked job. */
#define RELEASE_OFFSET	2048

#define RESULT_MAGIC	0xc0ffeeu

#define SEC_NS		1000000000ULL

/* Completion budget, the same one the scheduler tests use. */
#define WAIT_NS		(10 * SEC_NS)

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

/*
 * Wait for a syncobj and check that the fence it carries completed without an
 * error, so that a failed operation cannot pass as a completed one.
 */
static void assert_completed(int fd, uint32_t syncobj, const char *what)
{
	int fence, status;

	igt_assert_f(wait_done(fd, syncobj, WAIT_NS), "%s did not retire\n",
		     what);

	fence = syncobj_handle_to_fd(fd, syncobj,
				     DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE);
	status = sync_fence_status(fence);
	close(fence);

	igt_assert_f(status == 1, "%s failed (fence status %d)\n", what,
		     status);
}

/*
 * Map one page through the async bind queue and skip the calling subtest if
 * the driver has no async bind path at all, so that its absence does not read
 * as a chaining failure. Self-contained, so that the skip leaves nothing
 * allocated.
 */
static void require_async_vm_bind(int fd)
{
	uint32_t vm_id, syncobj;
	struct panthor_bo bo = {};
	struct drm_panthor_sync_op sync;
	struct drm_panthor_vm_bind_op op;
	struct drm_panthor_vm_bind bind;
	int ret, saved_errno;

	igt_panthor_vm_create(fd, &vm_id, 0);
	igt_panthor_bo_create(fd, &bo, PAGE_SIZE, 0, 0);
	syncobj = syncobj_create(fd, 0);
	sync = signal_op(syncobj);

	op = (struct drm_panthor_vm_bind_op){
		.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
		.bo_handle = bo.handle,
		.va = VA_A,
		.size = PAGE_SIZE,
		.syncs = DRM_PANTHOR_OBJ_ARRAY(1, &sync),
	};
	bind = (struct drm_panthor_vm_bind){
		.vm_id = vm_id,
		.flags = DRM_PANTHOR_VM_BIND_ASYNC,
		.ops = DRM_PANTHOR_OBJ_ARRAY(1, &op),
	};

	ret = igt_ioctl(fd, DRM_IOCTL_PANTHOR_VM_BIND, &bind);
	saved_errno = errno;

	if (ret == 0)
		assert_completed(fd, syncobj, "probe bind");

	syncobj_destroy(fd, syncobj);
	igt_panthor_free_bo(fd, &bo);
	igt_panthor_vm_destroy(fd, vm_id, 0);

	igt_require_f(ret == 0, "async VM_BIND unsupported (errno %d)\n",
		      saved_errno);
}

static void submit_stream(int fd, uint32_t group_handle, uint64_t stream_addr,
			  uint32_t stream_size,
			  struct drm_panthor_sync_op *syncs, uint32_t nsyncs)
{
	struct drm_panthor_queue_submit submit = {
		.queue_index = 0,
		.stream_size = stream_size,
		.stream_addr = stream_addr,
		.syncs = DRM_PANTHOR_OBJ_ARRAY(nsyncs, syncs),
	};
	struct drm_panthor_group_submit group_submit = {
		.group_handle = group_handle,
		.queue_submits = DRM_PANTHOR_OBJ_ARRAY(1, &submit),
	};

	igt_panthor_group_submit(fd, &group_submit, 0);
}

/* Store a 32-bit constant to a mapped address. Returns the stream size. */
static uint32_t emit_store32(uint64_t *instrs, uint64_t addr, uint32_t value)
{
	instrs[0] = cs_mov48(0, addr);
	instrs[1] = cs_mov32(2, value);
	instrs[2] = cs_sync_set32(0, 2);

	return 3 * sizeof(instrs[0]);
}

/*
 * A group running a command stream that spins until the host writes a nonzero
 * word through @release, so that the caller controls when the job's fence
 * signals.
 */
struct blocked_job {
	uint32_t vm_id;
	uint32_t group_handle;
	uint32_t stream_size;
	struct panthor_bo bo;
	volatile uint32_t *release;
};

static void blocked_job_init(int fd, struct blocked_job *job)
{
	uint64_t instrs[3];

	igt_panthor_vm_create(fd, &job->vm_id, 0);
	job->group_handle = igt_panthor_group_create_simple(fd, job->vm_id, 0);

	igt_panthor_bo_create_mapped(fd, &job->bo, PAGE_SIZE, 0, 0);
	igt_panthor_vm_bind(fd, job->vm_id, job->bo.handle, VA_A, job->bo.size,
			    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

	job->release = (volatile uint32_t *)((uint8_t *)job->bo.map +
					     RELEASE_OFFSET);
	*job->release = 0;

	instrs[0] = cs_mov48(0, VA_A + RELEASE_OFFSET);
	instrs[1] = cs_mov32(2, 0);
	instrs[2] = cs_sync_wait32(0, 2, CS_CONDITION_GT);
	memcpy(job->bo.map, instrs, sizeof(instrs));
	job->stream_size = sizeof(instrs);
}

/* Returns the raw GROUP_SUBMIT result, so callers can probe for support. */
static int blocked_job_submit(int fd, struct blocked_job *job,
			      struct drm_panthor_sync_op *syncs, uint32_t nsyncs)
{
	struct drm_panthor_queue_submit submit = {
		.queue_index = 0,
		.stream_size = job->stream_size,
		.stream_addr = VA_A,
		.syncs = DRM_PANTHOR_OBJ_ARRAY(nsyncs, syncs),
	};
	struct drm_panthor_group_submit group_submit = {
		.group_handle = job->group_handle,
		.queue_submits = DRM_PANTHOR_OBJ_ARRAY(1, &submit),
	};

	return igt_ioctl(fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &group_submit);
}

static void blocked_job_fini(int fd, struct blocked_job *job)
{
	igt_panthor_group_destroy(fd, job->group_handle, 0);
	igt_panthor_free_bo(fd, &job->bo);
	igt_panthor_vm_destroy(fd, job->vm_id, 0);
}

int igt_main()
{
	int fd = -1;

	igt_fixture() {
		igt_panthor_skip_on_big_endian();
		fd = drm_open_driver(DRIVER_PANTHOR);
	}

	igt_describe("A VM_BIND_ASYNC operation array whose second map waits on "
		     "a syncobj the first map signals is accepted, and both "
		     "operations complete.");
	igt_subtest("vm-bind-array-chained-syncobj") {
		uint32_t vm_id, chained, done;
		struct panthor_bo bo_a = {}, bo_b = {};
		struct drm_panthor_sync_op first_syncs[1], second_syncs[2];
		struct drm_panthor_vm_bind_op ops[2];
		struct drm_panthor_vm_bind bind;

		require_async_vm_bind(fd);

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_panthor_bo_create(fd, &bo_a, PAGE_SIZE, 0, 0);
		igt_panthor_bo_create(fd, &bo_b, PAGE_SIZE, 0, 0);

		chained = syncobj_create(fd, 0);
		done = syncobj_create(fd, 0);

		/* The second map waits on what the first map signals. */
		first_syncs[0] = signal_op(chained);
		second_syncs[0] = wait_op(chained);
		second_syncs[1] = signal_op(done);

		ops[0] = (struct drm_panthor_vm_bind_op){
			.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
			.bo_handle = bo_a.handle,
			.va = VA_A,
			.size = PAGE_SIZE,
			.syncs = DRM_PANTHOR_OBJ_ARRAY(1, first_syncs),
		};
		ops[1] = (struct drm_panthor_vm_bind_op){
			.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
			.bo_handle = bo_b.handle,
			.va = VA_B,
			.size = PAGE_SIZE,
			.syncs = DRM_PANTHOR_OBJ_ARRAY(2, second_syncs),
		};
		bind = (struct drm_panthor_vm_bind){
			.vm_id = vm_id,
			.flags = DRM_PANTHOR_VM_BIND_ASYNC,
			.ops = DRM_PANTHOR_OBJ_ARRAY(2, ops),
		};

		igt_assert_f(igt_ioctl(fd, DRM_IOCTL_PANTHOR_VM_BIND, &bind) == 0,
			     "chained bind array rejected (errno %d)\n", errno);

		assert_completed(fd, chained, "first map");
		assert_completed(fd, done, "second map");

		syncobj_destroy(fd, chained);
		syncobj_destroy(fd, done);
		igt_panthor_free_bo(fd, &bo_a);
		igt_panthor_free_bo(fd, &bo_b);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("A GROUP_SUBMIT batch whose first job waits on an "
		     "already-signaled syncobj that a later job of the same "
		     "batch signals is accepted, and both jobs complete.");
	igt_subtest("group-submit-batch-wait-on-later-signal") {
		uint32_t vm_id, group_handle;
		uint32_t shared, first_done, second_done;
		struct drm_panthor_sync_op first_syncs[2], second_syncs[2];
		struct drm_panthor_queue_submit submits[2];
		struct drm_panthor_group_submit group_submit;

		igt_panthor_vm_create(fd, &vm_id, 0);
		group_handle = igt_panthor_group_create_simple(fd, vm_id, 0);

		/* The wait has a fence to resolve against only if it is here. */
		shared = syncobj_create(fd, DRM_SYNCOBJ_CREATE_SIGNALED);
		first_done = syncobj_create(fd, 0);
		second_done = syncobj_create(fd, 0);

		first_syncs[0] = wait_op(shared);
		first_syncs[1] = signal_op(first_done);
		second_syncs[0] = signal_op(shared);
		second_syncs[1] = signal_op(second_done);

		/*
		 * Both jobs run on queue 0, so a wait resolved against the
		 * later job's own fence stalls the queue instead of passing.
		 */
		submits[0] = (struct drm_panthor_queue_submit){
			.queue_index = 0,
			.syncs = DRM_PANTHOR_OBJ_ARRAY(2, first_syncs),
		};
		submits[1] = (struct drm_panthor_queue_submit){
			.queue_index = 0,
			.syncs = DRM_PANTHOR_OBJ_ARRAY(2, second_syncs),
		};
		group_submit = (struct drm_panthor_group_submit){
			.group_handle = group_handle,
			.queue_submits = DRM_PANTHOR_OBJ_ARRAY(2, submits),
		};

		igt_assert_f(igt_ioctl(fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT,
				       &group_submit) == 0,
			     "batch rejected (errno %d)\n", errno);

		assert_completed(fd, first_done, "first job");
		assert_completed(fd, second_done, "second job");

		syncobj_destroy(fd, shared);
		syncobj_destroy(fd, first_done);
		syncobj_destroy(fd, second_done);
		igt_panthor_group_destroy(fd, group_handle, 0);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("A job that waits on the syncobj an asynchronous bind "
		     "signals runs against the mapping that bind created.");
	igt_subtest("bind-fence-gates-submit") {
		uint32_t vm_id, group_handle, bound, done;
		struct panthor_bo cmd = {}, result = {};
		struct drm_panthor_sync_op sync, job_syncs[2];
		struct drm_panthor_vm_bind_op op;
		struct drm_panthor_vm_bind bind;
		volatile uint32_t *written;
		uint64_t instrs[3];
		uint32_t stream_size;

		require_async_vm_bind(fd);

		igt_panthor_vm_create(fd, &vm_id, 0);
		group_handle = igt_panthor_group_create_simple(fd, vm_id, 0);

		igt_panthor_bo_create_mapped(fd, &cmd, PAGE_SIZE, 0, 0);
		igt_panthor_bo_create_mapped(fd, &result, PAGE_SIZE, 0, 0);

		stream_size = emit_store32(instrs, VA_B, RESULT_MAGIC);
		memcpy(cmd.map, instrs, stream_size);

		written = result.map;
		*written = 0;

		igt_panthor_vm_bind(fd, vm_id, cmd.handle, VA_A, cmd.size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		bound = syncobj_create(fd, 0);
		done = syncobj_create(fd, 0);

		sync = signal_op(bound);
		op = (struct drm_panthor_vm_bind_op){
			.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP |
				 DRM_PANTHOR_VM_BIND_OP_MAP_UNCACHED,
			.bo_handle = result.handle,
			.va = VA_B,
			.size = result.size,
			.syncs = DRM_PANTHOR_OBJ_ARRAY(1, &sync),
		};
		bind = (struct drm_panthor_vm_bind){
			.vm_id = vm_id,
			.flags = DRM_PANTHOR_VM_BIND_ASYNC,
			.ops = DRM_PANTHOR_OBJ_ARRAY(1, &op),
		};
		igt_assert_f(igt_ioctl(fd, DRM_IOCTL_PANTHOR_VM_BIND, &bind) == 0,
			     "async bind rejected (errno %d)\n", errno);

		job_syncs[0] = wait_op(bound);
		job_syncs[1] = signal_op(done);
		submit_stream(fd, group_handle, VA_A, stream_size, job_syncs, 2);

		assert_completed(fd, done, "gated job");
		igt_assert_eq_u32(*written, RESULT_MAGIC);

		syncobj_destroy(fd, bound);
		syncobj_destroy(fd, done);
		igt_panthor_group_destroy(fd, group_handle, 0);
		igt_panthor_free_bo(fd, &cmd);
		igt_panthor_free_bo(fd, &result);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("An asynchronous unbind that waits on the syncobj a job "
		     "signals stays pending until that job retires.");
	igt_subtest("submit-fence-gates-bind") {
		struct drm_panthor_sync_op job_sync, bind_syncs[2];
		struct drm_panthor_vm_bind_op op;
		struct drm_panthor_vm_bind bind;
		struct panthor_bo scratch = {};
		uint32_t job_done, unbound;
		struct blocked_job job;
		bool early;

		require_async_vm_bind(fd);

		blocked_job_init(fd, &job);

		igt_panthor_bo_create(fd, &scratch, PAGE_SIZE, 0, 0);
		igt_panthor_vm_bind(fd, job.vm_id, scratch.handle, VA_C,
				    scratch.size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		job_done = syncobj_create(fd, 0);
		unbound = syncobj_create(fd, 0);

		job_sync = signal_op(job_done);
		igt_assert_f(blocked_job_submit(fd, &job, &job_sync, 1) == 0,
			     "blocked job rejected (errno %d)\n", errno);

		bind_syncs[0] = wait_op(job_done);
		bind_syncs[1] = signal_op(unbound);
		op = (struct drm_panthor_vm_bind_op){
			.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP,
			.va = VA_C,
			.size = scratch.size,
			.syncs = DRM_PANTHOR_OBJ_ARRAY(2, bind_syncs),
		};
		bind = (struct drm_panthor_vm_bind){
			.vm_id = job.vm_id,
			.flags = DRM_PANTHOR_VM_BIND_ASYNC,
			.ops = DRM_PANTHOR_OBJ_ARRAY(1, &op),
		};
		igt_assert_f(igt_ioctl(fd, DRM_IOCTL_PANTHOR_VM_BIND, &bind) == 0,
			     "gated unbind rejected (errno %d)\n", errno);

		early = wait_done(fd, unbound, SEC_NS);

		/*
		 * A failed assertion would leave the stream spinning until the
		 * job timeout kills the group, landing in the middle of the
		 * next subtest, so release before asserting.
		 */
		*job.release = 1;
		igt_assert_f(!early,
			     "unbind retired while the job was still blocked\n");

		assert_completed(fd, job_done, "blocked job");
		assert_completed(fd, unbound, "gated unbind");

		syncobj_destroy(fd, job_done);
		syncobj_destroy(fd, unbound);
		igt_panthor_free_bo(fd, &scratch);
		blocked_job_fini(fd, &job);
	}

	igt_describe("A syncobj exported from one file descriptor and imported "
		     "into another signals on the importing descriptor only "
		     "once the exporting one's job retires.");
	igt_subtest("syncobj-cross-fd-transfer") {
		struct drm_panthor_sync_op sync;
		uint32_t local, remote;
		struct blocked_job job;
		int other_fd, sync_fd;
		bool early;

		blocked_job_init(fd, &job);

		local = syncobj_create(fd, 0);
		sync = signal_op(local);
		igt_assert_f(blocked_job_submit(fd, &job, &sync, 1) == 0,
			     "blocked job rejected (errno %d)\n", errno);

		other_fd = drm_reopen_driver(fd);
		sync_fd = syncobj_handle_to_fd(fd, local, 0);
		remote = syncobj_fd_to_handle(other_fd, sync_fd, 0);
		close(sync_fd);

		early = wait_done(other_fd, remote, SEC_NS);

		*job.release = 1;
		igt_assert_f(!early, "imported syncobj signaled early\n");

		assert_completed(other_fd, remote, "imported syncobj");

		syncobj_destroy(other_fd, remote);
		close(other_fd);
		syncobj_destroy(fd, local);
		blocked_job_fini(fd, &job);
	}

	igt_describe("A timeline point transferred into a binary syncobj and "
		     "imported into another file descriptor signals there only "
		     "once the job that signals the point retires.");
	igt_subtest("syncobj-timeline-to-binary-cross-fd") {
		uint32_t timeline, binary, remote;
		struct drm_panthor_sync_op sync;
		struct blocked_job job;
		int other_fd, sync_fd;
		bool early;

		blocked_job_init(fd, &job);

		timeline = syncobj_create(fd, 0);
		sync = (struct drm_panthor_sync_op){
			.flags = DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ |
				 DRM_PANTHOR_SYNC_OP_SIGNAL,
			.handle = timeline,
			.timeline_value = 1,
		};

		if (blocked_job_submit(fd, &job, &sync, 1)) {
			int saved_errno = errno;

			syncobj_destroy(fd, timeline);
			blocked_job_fini(fd, &job);
			igt_skip("timeline sync operations unsupported "
				 "(errno %d)\n", saved_errno);
		}

		binary = syncobj_create(fd, 0);
		syncobj_timeline_to_binary(fd, binary, timeline, 1, 0);

		other_fd = drm_reopen_driver(fd);
		sync_fd = syncobj_handle_to_fd(fd, binary, 0);
		remote = syncobj_fd_to_handle(other_fd, sync_fd, 0);
		close(sync_fd);

		early = wait_done(other_fd, remote, SEC_NS);

		*job.release = 1;
		igt_assert_f(!early, "transferred point signaled early\n");

		assert_completed(other_fd, remote, "transferred timeline point");

		syncobj_destroy(other_fd, remote);
		close(other_fd);
		syncobj_destroy(fd, binary);
		syncobj_destroy(fd, timeline);
		blocked_job_fini(fd, &job);
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
