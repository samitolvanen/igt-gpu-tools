// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (C) 2025 Collabora Ltd.

#include <stdint.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "igt.h"
#include "igt_panthor.h"
#include "igt_syncobj.h"
#include "panthor_drm.h"

/* Completion budget, the same one the other panthor tests use. */
#define WAIT_NS (10 * NSEC_PER_SEC)

static size_t
issue_store_multiple(uint8_t *cs, uint64_t kernel_va, uint32_t constant)
{
	const uint8_t kernel_va_reg = 68;
	const uint8_t constant_reg = 70;
	uint64_t instrs[] = {
		/* MOV48: Load the source register ([r68; r69]) with the kernel address */
		cs_mov48(kernel_va_reg, kernel_va),
		/* MOV32: Load a known constant into r70 */
		cs_mov32(constant_reg, constant),
		/* STORE_MULTIPLE: Store the first register to the address pointed
		 * to by [r68; r69]
		 */
		cs_stm32(kernel_va_reg, constant_reg, 0),
		/* FLUSH all Wait for all cores */
		cs_wait(0xff, false),
		/* MOV32: Clear r70 to 0 */
		cs_mov32(constant_reg, 0),
		/* FLUSH_CACHE: Clean and invalidate all caches */
		cs_flush(CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
			 CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
			 CS_FLUSH_MODE_INVALIDATE,
			 0, constant_reg, 1),
		cs_wait(0xff, false),
	};

	memcpy(cs, instrs, sizeof(instrs));
	return sizeof(instrs);
}

static bool wait_signaled(int fd, uint32_t syncobj)
{
	struct timespec ts;
	int64_t deadline;

	igt_assert_eq(clock_gettime(CLOCK_MONOTONIC, &ts), 0);
	deadline = (int64_t)ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec + WAIT_NS;

	return syncobj_wait(fd, &syncobj, 1, deadline,
			    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL);
}

/*
 * Issue a GROUP_SUBMIT and return the raw ioctl result (0 on success, -1 on
 * failure with errno set).
 */
static int do_group_submit(int fd, uint32_t group_handle,
			   struct drm_panthor_queue_submit *submits,
			   uint32_t count)
{
	struct drm_panthor_group_submit group_submit = {
		.group_handle = group_handle,
		.queue_submits = DRM_PANTHOR_OBJ_ARRAY(count, submits),
	};

	return igt_ioctl(fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &group_submit);
}

int igt_main() {
	int fd = -1;

	igt_fixture() {
		igt_panthor_skip_on_big_endian();
		fd = drm_open_driver(DRIVER_PANTHOR);
	}

	igt_describe("Create and destroy a CSF group.");
	igt_subtest("group_create") {
		struct drm_panthor_vm_create vm_create = {};
		struct drm_panthor_vm_destroy vm_destroy = {};
		uint32_t group_handle;

		vm_create.flags = 0;
		do_ioctl(fd, DRM_IOCTL_PANTHOR_VM_CREATE, &vm_create);
		igt_assert_neq(vm_create.id, 0);

		group_handle = igt_panthor_group_create_simple(fd, vm_create.id, 0);
		igt_assert_neq(group_handle, 0);

		igt_panthor_group_destroy(fd, group_handle, 0);

		vm_destroy = (struct drm_panthor_vm_destroy) { .id = vm_create.id };
		do_ioctl(fd, DRM_IOCTL_PANTHOR_VM_DESTROY, &vm_destroy);
	}

	igt_describe("Submit a job to a group and wait for completion. "
		     "The job writes a known value to a buffer object that is then "
		     "mmaped and checked.");
	igt_subtest("group_submit") {
		uint32_t vm_id;
		uint32_t group_handle;
		struct panthor_bo cmd_buf_bo = {};
		struct panthor_bo result_bo = {};
		uint64_t command_stream_gpu_addr;
		uint32_t command_stream_size;
		uint64_t result_gpu_addr;
		uint32_t syncobj_handle;
		const int INITIAL_VA = 0x1000000;

		igt_panthor_vm_create(fd, &vm_id, 0);

		igt_panthor_bo_create_mapped(fd, &cmd_buf_bo, 4096, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, cmd_buf_bo.handle, INITIAL_VA,
				    cmd_buf_bo.size, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);
		command_stream_gpu_addr = INITIAL_VA;

		/* Create the BO to receive the result of the store. */
		igt_panthor_bo_create_mapped(fd, &result_bo, 4096, 0, 0);
		/* Also bind the result BO. */
		igt_panthor_vm_bind(fd, vm_id, result_bo.handle, INITIAL_VA + 4096,
				    result_bo.size, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);
		result_gpu_addr = INITIAL_VA + 4096;

		command_stream_size = issue_store_multiple(cmd_buf_bo.map, result_gpu_addr, 0xdeadbeef);

		group_handle = igt_panthor_group_create_simple(fd, vm_id, 0);
		igt_assert_neq(group_handle, 0);

		syncobj_handle = syncobj_create(fd, 0);

		igt_panthor_group_submit_simple(fd, group_handle, 0, command_stream_gpu_addr, command_stream_size, syncobj_handle, 0);

		igt_assert(syncobj_wait(fd, &syncobj_handle, 1, INT64_MAX, 0, NULL));

		igt_assert_eq(*(uint32_t *)result_bo.map, 0xdeadbeef);

		syncobj_destroy(fd, syncobj_handle);

		igt_panthor_group_destroy(fd, group_handle, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);

		igt_panthor_free_bo(fd, &cmd_buf_bo);
		igt_panthor_free_bo(fd, &result_bo);
	}

	igt_describe("A GROUP_SUBMIT carrying no queue submits is a no-op, and "
		     "a queue submit with an empty command stream is a "
		     "synchronization point.");
	igt_subtest("group_submit_empty") {
		uint32_t vm_id, group_handle, syncobj;
		struct drm_panthor_queue_submit submit;
		struct drm_panthor_sync_op sync;

		igt_panthor_vm_create(fd, &vm_id, 0);
		group_handle = igt_panthor_group_create_simple(fd, vm_id, 0);
		igt_assert_neq(group_handle, 0);

		igt_assert_eq(do_group_submit(fd, group_handle, NULL, 0), 0);

		syncobj = syncobj_create(fd, 0);
		sync = (struct drm_panthor_sync_op){
			.flags = DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ |
				 DRM_PANTHOR_SYNC_OP_SIGNAL,
			.handle = syncobj,
		};
		submit = (struct drm_panthor_queue_submit){
			.queue_index = 0,
			.syncs = DRM_PANTHOR_OBJ_ARRAY(1, &sync),
		};
		igt_assert_eq(do_group_submit(fd, group_handle, &submit, 1), 0);
		igt_assert(wait_signaled(fd, syncobj));

		/* A stream address and a stream size are all-or-nothing. */
		submit = (struct drm_panthor_queue_submit){
			.queue_index = 0,
			.stream_addr = 0x1000000,
		};
		igt_assert_eq(do_group_submit(fd, group_handle, &submit, 1), -1);
		igt_assert_eq(errno, EINVAL);

		submit = (struct drm_panthor_queue_submit){
			.queue_index = 0,
			.stream_size = 8,
		};
		igt_assert_eq(do_group_submit(fd, group_handle, &submit, 1), -1);
		igt_assert_eq(errno, EINVAL);

		/* The group was created with a single queue. */
		submit = (struct drm_panthor_queue_submit){
			.queue_index = 1,
		};
		igt_assert_eq(do_group_submit(fd, group_handle, &submit, 1), -1);
		igt_assert_eq(errno, EINVAL);

		{
			struct drm_panthor_group_submit group_submit = {
				.group_handle = group_handle,
				.pad = 1,
			};
			do_ioctl_err(fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT,
				     &group_submit, EINVAL);
		}

		syncobj_destroy(fd, syncobj);
		igt_panthor_group_destroy(fd, group_handle, 0);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
