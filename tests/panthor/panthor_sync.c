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

	igt_fixture() {
		drm_close_driver(fd);
	}
}
