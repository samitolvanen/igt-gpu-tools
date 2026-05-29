// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (C) 2025 Collabora Ltd.
// SPDX-FileCopyrightText: Copyright (C) 2026 Google LLC

#include "igt.h"
#include "igt_core.h"
#include "igt_panthor.h"
#include "igt_syncobj.h"
#include "drmtest.h"
#include "ioctl_wrappers.h"
#include "panthor_drm.h"

#define PAGE_SIZE 4096ULL

/* A page-aligned GPU VA inside the user range, unlikely to collide. */
#define TEST_VA 0x100000ULL

/*
 * Issue a single synchronous VM_BIND op and return the raw ioctl result
 * (0 on success, -1 on failure with errno set).
 */
static int do_sync_bind_op(int fd, uint32_t vm_id,
			   struct drm_panthor_vm_bind_op *op)
{
	struct drm_panthor_vm_bind bind = {
		.vm_id = vm_id,
		.flags = 0,
		.ops = DRM_PANTHOR_OBJ_ARRAY(1, op),
	};

	return igt_ioctl(fd, DRM_IOCTL_PANTHOR_VM_BIND, &bind);
}

/*
 * Issue a single async VM_BIND op carrying a signal syncobj, drain the syncobj
 * if the bind was accepted, and return the entry-time ioctl result (0 on
 * success, -1 on failure with errno set). Only alignment and structural checks
 * are guaranteed at ioctl entry; semantic checks (range/bounds) may be deferred
 * to the async bind worker and surface through the bind fence instead.
 */
static int do_async_bind_op(int fd, uint32_t vm_id,
			    struct drm_panthor_vm_bind_op *op)
{
	uint32_t syncobj = syncobj_create(fd, 0);
	struct drm_panthor_sync_op sig = {
		.flags = DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ |
			 DRM_PANTHOR_SYNC_OP_SIGNAL,
		.handle = syncobj,
	};
	struct drm_panthor_vm_bind bind;
	int ret, saved_errno;

	op->syncs = (struct drm_panthor_obj_array)DRM_PANTHOR_OBJ_ARRAY(1, &sig);

	bind = (struct drm_panthor_vm_bind){
		.vm_id = vm_id,
		.flags = DRM_PANTHOR_VM_BIND_ASYNC,
		.ops = DRM_PANTHOR_OBJ_ARRAY(1, op),
	};
	ret = igt_ioctl(fd, DRM_IOCTL_PANTHOR_VM_BIND, &bind);
	saved_errno = errno;

	/*
	 * Drain the syncobj if the bind was accepted, so a later op on the same
	 * VM does not race against an in-flight async bind.
	 */
	if (ret == 0) {
		uint64_t abs;
		struct timespec ts;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		abs = (uint64_t)ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec +
		      NSEC_PER_SEC;
		syncobj_wait(fd, &syncobj, 1, abs,
			     DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL);
	}

	syncobj_destroy(fd, syncobj);

	/* Clear the per-op syncs so the caller can reuse the op struct. */
	op->syncs = (struct drm_panthor_obj_array){ 0 };
	errno = saved_errno;
	return ret;
}

int igt_main() {
	int fd = -1;

	igt_fixture() {
		igt_panthor_skip_on_big_endian();
		fd = drm_open_driver(DRIVER_PANTHOR);
	}

	igt_describe("Create and destroy a VM");
	igt_subtest("vm_create_destroy") {
		uint32_t vm_id;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert_neq(vm_id, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_subtest("vm_destroy_invalid") {
		igt_panthor_vm_destroy(fd, 0xdeadbeef, EINVAL);
	}

	igt_describe("Test the VM_BIND API synchronously");
	igt_subtest("vm_bind") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = 0x1000;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert_neq(vm_id, 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle,
				    0x1000, 0x1000, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Test unbinding a previously bound range");
	igt_subtest("vm_unbind") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = 0x1000;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert_neq(vm_id, 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle,
				    0x1000, 0x1000, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0,
				    0x1000, 0x1000, DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Test unbinding an address range that was not previously bound");
	igt_subtest("vm_unbind_invalid_address") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = 0x1000;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert_neq(vm_id, 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);

		/* This was not bound previously*/
		igt_panthor_vm_bind(fd, vm_id, bo.handle,
				    0x1000, 0x1000, DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, EINVAL);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is identical to existing huge page mapping");
	igt_subtest("vm_unbind_identical_hugepage_single") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_2M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is identical to existing huge page mapping, but only a subset of the object's pages are mapped in the VM");
	igt_subtest("vm_unbind_identical_hugepage_single_partial") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_4M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is identical to existing multiple huge page mapping");
	igt_subtest("vm_unbind_identical_hugepage_multiple") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_1M * 6;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is identical to existing huge page mapping, but only part of the BO is mapped");
	igt_subtest("vm_unbind_identical_hugepage_offset") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_1M * 6;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind_offset(fd, vm_id, bo.handle, 0x200000,
					   SZ_4M, SZ_2M, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is left aligned subset of an existing huge page mapping");
	igt_subtest("vm_unbind_hugepage_leftaligned") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_4M;
		uint64_t unmap_size = SZ_8K;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, unmap_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is right aligned subset of an existing huge page mapping");
	igt_subtest("vm_unbind_hugepage_rightaligned") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_4M;
		uint64_t unmap_size = SZ_8K;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000 + bo_size - unmap_size, unmap_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is a superset of existing huge page mapping");
	igt_subtest("vm_unbind_hugepage_superset") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_2M;
		uint64_t unmap_size = SZ_1M * 5;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x100000, unmap_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is a subset of an existing mapping");
	igt_subtest("vm_unbind_hugepage_subset") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_4M;
		uint64_t unmap_size = SZ_8K;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000 + SZ_1M, unmap_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000 + SZ_2M + SZ_4K, unmap_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Perform successive unmaps over the remnants of an original multi huge page mapping");
	igt_subtest("vm_unbind_hugepage_successive") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_1M * 6;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x3fc000, 0x208000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x3fc000, 0x208000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, 0x4000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x401000, 0x4000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x4fb000, 0xA000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x3fb000, 0xA000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x3fc000, 0x4000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, 0x1000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Reject VM_BIND ops with bad alignment, overflow, or out-of-bounds ranges");
	igt_subtest("vm_bind_validation") {
		uint32_t vm_id;
		struct panthor_bo bo;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert_neq(vm_id, 0);
		igt_panthor_bo_create(fd, &bo, PAGE_SIZE, 0, 0);

		/* Unaligned va is rejected with EINVAL. */
		{
			struct drm_panthor_vm_bind_op op = {
				.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
				.bo_handle = bo.handle,
				.va = TEST_VA + 1,
				.size = PAGE_SIZE,
			};
			igt_assert_eq(do_sync_bind_op(fd, vm_id, &op), -1);
			igt_assert_eq(errno, EINVAL);
		}

		/* Unaligned size is rejected with EINVAL. */
		{
			struct drm_panthor_vm_bind_op op = {
				.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
				.bo_handle = bo.handle,
				.va = TEST_VA,
				.size = PAGE_SIZE + 1,
			};
			igt_assert_eq(do_sync_bind_op(fd, vm_id, &op), -1);
			igt_assert_eq(errno, EINVAL);
		}

		/* Unaligned bo_offset is rejected with EINVAL. */
		{
			struct drm_panthor_vm_bind_op op = {
				.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
				.bo_handle = bo.handle,
				.va = TEST_VA,
				.size = PAGE_SIZE,
				.bo_offset = 1,
			};
			igt_assert_eq(do_sync_bind_op(fd, vm_id, &op), -1);
			igt_assert_eq(errno, EINVAL);
		}

		/*
		 * va + size overflowing past UINT64_MAX is rejected. The errno
		 * differs between drivers (bounds check vs checked_add), so
		 * assert failure generically.
		 */
		{
			struct drm_panthor_vm_bind_op op = {
				.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
				.bo_handle = bo.handle,
				.va = UINT64_MAX - (PAGE_SIZE - 1),
				.size = PAGE_SIZE,
			};
			igt_assert_eq(do_sync_bind_op(fd, vm_id, &op), -1);
		}

		/* size > bo.size is rejected with EINVAL. */
		{
			struct drm_panthor_vm_bind_op op = {
				.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
				.bo_handle = bo.handle,
				.va = TEST_VA,
				.size = PAGE_SIZE * 2,
			};
			igt_assert_eq(do_sync_bind_op(fd, vm_id, &op), -1);
			igt_assert_eq(errno, EINVAL);
		}

		/* bo_offset + size > bo.size is rejected with EINVAL. */
		{
			struct drm_panthor_vm_bind_op op = {
				.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
				.bo_handle = bo.handle,
				.va = TEST_VA,
				.size = PAGE_SIZE,
				.bo_offset = PAGE_SIZE,
			};
			igt_assert_eq(do_sync_bind_op(fd, vm_id, &op), -1);
			igt_assert_eq(errno, EINVAL);
		}

		/* A bogus vm_id is rejected with EINVAL. */
		{
			struct drm_panthor_vm_bind_op op = {
				.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
				.bo_handle = bo.handle,
				.va = TEST_VA,
				.size = PAGE_SIZE,
			};
			igt_assert_eq(do_sync_bind_op(fd, 0xdeadbeef, &op), -1);
			igt_assert_eq(errno, EINVAL);
		}

		/*
		 * A bogus bo_handle on MAP is rejected. The errno differs
		 * between drivers (EINVAL vs ENOENT), so assert failure
		 * generically.
		 */
		{
			struct drm_panthor_vm_bind_op op = {
				.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
				.bo_handle = 0xdeadbeef,
				.va = TEST_VA,
				.size = PAGE_SIZE,
			};
			igt_assert_eq(do_sync_bind_op(fd, vm_id, &op), -1);
		}

		igt_panthor_free_bo(fd, &bo);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Exercise VM_BIND ops-array stride/count handling");
	igt_subtest("vm_bind_array") {
		uint32_t vm_id;
		struct panthor_bo bo;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert_neq(vm_id, 0);
		igt_panthor_bo_create(fd, &bo, PAGE_SIZE, 0, 0);

		/* stride < sizeof(op) is rejected with EINVAL. */
		{
			struct drm_panthor_vm_bind_op op = {
				.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
				.bo_handle = bo.handle,
				.va = TEST_VA,
				.size = PAGE_SIZE,
			};
			struct drm_panthor_vm_bind bind = {
				.vm_id = vm_id,
				.ops = {
					.count = 1,
					.stride = sizeof(op) - 8,
					.array = to_user_pointer(&op),
				},
			};
			do_ioctl_err(fd, DRM_IOCTL_PANTHOR_VM_BIND, &bind, EINVAL);
		}

		/* stride > sizeof(op) with a zero tail is accepted and maps. */
		{
			struct {
				struct drm_panthor_vm_bind_op op;
				uint8_t tail[16];
			} padded = {
				.op = {
					.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
					.bo_handle = bo.handle,
					.va = TEST_VA,
					.size = PAGE_SIZE,
				},
				.tail = { 0 },
			};
			struct drm_panthor_vm_bind bind = {
				.vm_id = vm_id,
				.ops = {
					.count = 1,
					.stride = sizeof(padded),
					.array = to_user_pointer(&padded),
				},
			};
			do_ioctl(fd, DRM_IOCTL_PANTHOR_VM_BIND, &bind);

			/* Tear down the mapping we just made. */
			igt_panthor_vm_bind(fd, vm_id, 0, TEST_VA, PAGE_SIZE,
					    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		}

		/* stride > sizeof(op) with a nonzero tail is rejected with E2BIG. */
		{
			struct {
				struct drm_panthor_vm_bind_op op;
				uint8_t tail[16];
			} padded = {
				.op = {
					.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
					.bo_handle = bo.handle,
					.va = TEST_VA,
					.size = PAGE_SIZE,
				},
				.tail = { 1 },
			};
			struct drm_panthor_vm_bind bind = {
				.vm_id = vm_id,
				.ops = {
					.count = 1,
					.stride = sizeof(padded),
					.array = to_user_pointer(&padded),
				},
			};
			do_ioctl_err(fd, DRM_IOCTL_PANTHOR_VM_BIND, &bind, E2BIG);
		}

		/* count == 0 is a no-op success. */
		{
			struct drm_panthor_vm_bind bind = {
				.vm_id = vm_id,
				.ops = {
					.count = 0,
					.stride = sizeof(struct drm_panthor_vm_bind_op),
					.array = 0,
				},
			};
			do_ioctl(fd, DRM_IOCTL_PANTHOR_VM_BIND, &bind);
		}

		/* Non-empty per-op syncs on a synchronous bind are rejected with EINVAL. */
		{
			uint32_t syncobj = syncobj_create(fd, 0);
			struct drm_panthor_sync_op sync = {
				.flags = DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ,
				.handle = syncobj,
			};
			struct drm_panthor_vm_bind_op op = {
				.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
				.bo_handle = bo.handle,
				.va = TEST_VA,
				.size = PAGE_SIZE,
				.syncs = DRM_PANTHOR_OBJ_ARRAY(1, &sync),
			};
			struct drm_panthor_vm_bind bind = {
				.vm_id = vm_id,
				.ops = DRM_PANTHOR_OBJ_ARRAY(1, &op),
			};
			do_ioctl_err(fd, DRM_IOCTL_PANTHOR_VM_BIND, &bind, EINVAL);
			syncobj_destroy(fd, syncobj);
		}

		igt_panthor_free_bo(fd, &bo);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Async VM_BIND rejects misaligned bind ops at ioctl entry");
	igt_subtest("vm_bind_async_validation") {
		uint32_t vm_id;
		struct panthor_bo bo;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert_neq(vm_id, 0);
		igt_panthor_bo_create(fd, &bo, PAGE_SIZE, 0, 0);

		/*
		 * An aligned MAP then UNMAP via the async path. Some drivers
		 * reject DRM_PANTHOR_VM_BIND_ASYNC entirely (e.g. ENOTSUPP); in
		 * that case skip, as the rejection-path coverage below would
		 * only re-test the same blanket rejection.
		 */
		{
			struct drm_panthor_vm_bind_op map = {
				.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
				.bo_handle = bo.handle,
				.va = TEST_VA,
				.size = PAGE_SIZE,
			};
			struct drm_panthor_vm_bind_op unmap = {
				.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP,
				.va = TEST_VA,
				.size = PAGE_SIZE,
			};
			int ret = do_async_bind_op(fd, vm_id, &map);

			igt_require_f(ret == 0,
				      "async VM_BIND unsupported (errno %d)\n",
				      errno);

			igt_assert_eq(do_async_bind_op(fd, vm_id, &unmap), 0);
		}

		/* Unaligned va is rejected at ioctl entry with EINVAL. */
		{
			struct drm_panthor_vm_bind_op op = {
				.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
				.bo_handle = bo.handle,
				.va = TEST_VA + 1,
				.size = PAGE_SIZE,
			};
			igt_assert_eq(do_async_bind_op(fd, vm_id, &op), -1);
			igt_assert_eq(errno, EINVAL);
		}

		/* Unaligned size is rejected with EINVAL. */
		{
			struct drm_panthor_vm_bind_op op = {
				.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
				.bo_handle = bo.handle,
				.va = TEST_VA,
				.size = PAGE_SIZE + 1,
			};
			igt_assert_eq(do_async_bind_op(fd, vm_id, &op), -1);
			igt_assert_eq(errno, EINVAL);
		}

		/* Unaligned bo_offset is rejected with EINVAL. */
		{
			struct drm_panthor_vm_bind_op op = {
				.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
				.bo_handle = bo.handle,
				.va = TEST_VA,
				.size = PAGE_SIZE,
				.bo_offset = 1,
			};
			igt_assert_eq(do_async_bind_op(fd, vm_id, &op), -1);
			igt_assert_eq(errno, EINVAL);
		}

		igt_panthor_free_bo(fd, &bo);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
