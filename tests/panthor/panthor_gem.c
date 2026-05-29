// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (C) 2025 Collabora Ltd.
// SPDX-FileCopyrightText: Copyright (C) 2026 Google LLC

#include <sys/mman.h>
#include <unistd.h>

#include "igt.h"
#include "igt_core.h"
#include "igt_panthor.h"
#include "drmtest.h"
#include "ioctl_wrappers.h"

#define PAGE_SIZE 4096ULL

/*
 * Issue a single BO_SYNC op and return the raw ioctl result (0 on success,
 * -1 on failure with errno set).
 */
static int do_bo_sync_op(int fd, struct drm_panthor_bo_sync_op *op)
{
	struct drm_panthor_bo_sync sync = {
		.ops = DRM_PANTHOR_OBJ_ARRAY(1, op),
	};

	return igt_ioctl(fd, DRM_IOCTL_PANTHOR_BO_SYNC, &sync);
}

/*
 * Probe whether the running driver implements DRM_PANTHOR_BO_SYNC. A FLUSH on
 * a freshly-created (write-combined) BO is a no-op and must succeed on a driver
 * that supports the ioctl; a driver without it fails the ioctl (e.g. ENOTTY).
 * Returns true when BO_SYNC is supported.
 */
static bool has_bo_sync(int fd)
{
	struct panthor_bo bo;
	struct drm_panthor_bo_sync_op op;
	bool supported;

	igt_panthor_bo_create(fd, &bo, PAGE_SIZE, 0, 0);

	op = (struct drm_panthor_bo_sync_op){
		.handle = bo.handle,
		.type = DRM_PANTHOR_BO_SYNC_CPU_CACHE_FLUSH,
		.size = PAGE_SIZE,
	};
	supported = do_bo_sync_op(fd, &op) == 0;

	igt_panthor_free_bo(fd, &bo);
	return supported;
}

/*
 * Probe whether the running driver accepts DRM_PANTHOR_BO_WB_MMAP at creation
 * time. Returns true when a WB-cacheable BO can be created.
 */
static bool has_wb_mmap(int fd)
{
	struct drm_panthor_bo_create create = {
		.size = PAGE_SIZE,
		.flags = DRM_PANTHOR_BO_WB_MMAP,
	};
	bool supported;

	supported = igt_ioctl(fd, DRM_IOCTL_PANTHOR_BO_CREATE, &create) == 0;
	if (supported)
		gem_close(fd, create.handle);

	return supported;
}

int igt_main() {
	int fd = -1;

	igt_fixture() {
		igt_panthor_skip_on_big_endian();
		fd = drm_open_driver(DRIVER_PANTHOR);
	}

	igt_describe("Create a buffer object");
	igt_subtest("bo_create") {
		struct panthor_bo bo;

		igt_panthor_bo_create(fd, &bo, 4096, 0, 0);
		igt_assert_neq(bo.handle, 0);

		igt_panthor_free_bo(fd, &bo);
	}

	igt_describe("Create a fake mmap offset for a buffer object");
	igt_subtest("bo_mmap_offset") {
		struct panthor_bo bo;
		uint64_t mmap_offset;

		igt_panthor_bo_create(fd, &bo, 4096, 0, 0);
		igt_assert_neq(bo.handle, 0);

		mmap_offset = igt_panthor_bo_mmap_offset(fd, bo.handle, 0);
		igt_assert_neq(mmap_offset, 0);

		igt_panthor_free_bo(fd, &bo);
	}

	igt_describe("Same as bo_mmap_offset but with an invalid handle");
	igt_subtest("bo_mmap_offset_invalid_handle") {
		struct panthor_bo bo;
		uint64_t mmap_offset;

		igt_panthor_bo_create(fd, &bo, 4096, 0, 0);
		igt_assert_neq(bo.handle, 0);

		mmap_offset = igt_panthor_bo_mmap_offset(fd, 0xdeadbeef, ENOENT);
		igt_assert_eq(mmap_offset, 0);

		igt_panthor_free_bo(fd, &bo);
	}

	igt_describe_f("Create a buffer object whose size is not page-aligned, and check "
		       "that the allocated size is rounded up to the next page size (%" PRIu64 ").",
		       (uint64_t)getpagesize() * 2);
	igt_subtest("bo_create_round_size") {
		struct panthor_bo bo;
		uint64_t expected_size = getpagesize() * 2;

		igt_panthor_bo_create(fd, &bo, 5000, 0, 0);
		igt_assert_neq(bo.handle, 0);
		igt_assert_eq(bo.size, expected_size);

		igt_panthor_free_bo(fd, &bo);
	}

	igt_describe_f("Check zero-ing of buffer at creation time");
	igt_subtest("bo_zeroed") {
		struct panthor_bo bo;

		igt_panthor_bo_create_mapped(fd, &bo, getpagesize(), 0, 0);
		igt_assert(bo.map);
		igt_assert_eq(*((uint32_t *)bo.map), 0);

		igt_panthor_free_bo(fd, &bo);
	}

	igt_describe("Reject invalid BO_CREATE arguments not covered by bo_create");
	igt_subtest("bo_create_validation") {
		struct drm_panthor_bo_create args;
		uint32_t vm_id;

		/* size 0 is rejected. */
		args = (struct drm_panthor_bo_create){ .size = 0 };
		do_ioctl_err(fd, DRM_IOCTL_PANTHOR_BO_CREATE, &args, EINVAL);

		/* An unknown flag bit is rejected. NO_MMAP=1<<0, WB_MMAP=1<<1. */
		args = (struct drm_panthor_bo_create){
			.size = PAGE_SIZE,
			.flags = (1u << 31),
		};
		do_ioctl_err(fd, DRM_IOCTL_PANTHOR_BO_CREATE, &args, EINVAL);

		/* A nonzero pad is rejected. */
		args = (struct drm_panthor_bo_create){
			.size = PAGE_SIZE,
			.pad = 1,
		};
		do_ioctl_err(fd, DRM_IOCTL_PANTHOR_BO_CREATE, &args, EINVAL);

		/* A bogus exclusive_vm_id is rejected. */
		args = (struct drm_panthor_bo_create){
			.size = PAGE_SIZE,
			.exclusive_vm_id = 0xdeadbeef,
		};
		do_ioctl_err(fd, DRM_IOCTL_PANTHOR_BO_CREATE, &args, EINVAL);

		/* A valid exclusive_vm_id is accepted. */
		igt_panthor_vm_create(fd, &vm_id, 0);
		args = (struct drm_panthor_bo_create){
			.size = PAGE_SIZE,
			.exclusive_vm_id = vm_id,
		};
		do_ioctl(fd, DRM_IOCTL_PANTHOR_BO_CREATE, &args);
		gem_close(fd, args.handle);
		igt_panthor_vm_destroy(fd, vm_id, 0);

		/*
		 * UINT64_MAX overflows page rounding. Both drivers reject it,
		 * but the errno is not guaranteed identical, so assert failure
		 * generically.
		 */
		args = (struct drm_panthor_bo_create){ .size = UINT64_MAX };
		igt_assert_lt(igt_ioctl(fd, DRM_IOCTL_PANTHOR_BO_CREATE, &args), 0);
	}

	igt_describe("Exercise the BO_SYNC ioctl edge cases");
	igt_subtest("bo_sync") {
		struct panthor_bo bo;

		igt_require(has_bo_sync(fd));

		igt_panthor_bo_create(fd, &bo, PAGE_SIZE, 0, 0);

		/* FLUSH on a write-combined BO is a no-op success. */
		{
			struct drm_panthor_bo_sync_op op = {
				.handle = bo.handle,
				.type = DRM_PANTHOR_BO_SYNC_CPU_CACHE_FLUSH,
				.size = PAGE_SIZE,
			};
			igt_assert_eq(do_bo_sync_op(fd, &op), 0);
		}

		/* size == 0 is a no-op success. */
		{
			struct drm_panthor_bo_sync_op op = {
				.handle = bo.handle,
				.type = DRM_PANTHOR_BO_SYNC_CPU_CACHE_FLUSH,
				.size = 0,
			};
			igt_assert_eq(do_bo_sync_op(fd, &op), 0);
		}

		/* A bogus type value is rejected with EINVAL. */
		{
			struct drm_panthor_bo_sync_op op = {
				.handle = bo.handle,
				.type = 0xdeadbeef,
				.size = PAGE_SIZE,
			};
			igt_assert_eq(do_bo_sync_op(fd, &op), -1);
			igt_assert_eq(errno, EINVAL);
		}

		/*
		 * A bogus handle is rejected. Both drivers reject, but the
		 * errno may differ (ENOENT vs EINVAL) through the DRM core, so
		 * assert failure generically.
		 */
		{
			struct drm_panthor_bo_sync_op op = {
				.handle = 0xdeadbeef,
				.type = DRM_PANTHOR_BO_SYNC_CPU_CACHE_FLUSH,
				.size = PAGE_SIZE,
			};
			igt_assert_eq(do_bo_sync_op(fd, &op), -1);
		}

		/* stride < sizeof(op) is rejected with EINVAL. */
		{
			struct drm_panthor_bo_sync_op op = {
				.handle = bo.handle,
				.type = DRM_PANTHOR_BO_SYNC_CPU_CACHE_FLUSH,
				.size = PAGE_SIZE,
			};
			struct drm_panthor_bo_sync sync = {
				.ops = {
					.count = 1,
					.stride = sizeof(op) - 4,
					.array = to_user_pointer(&op),
				},
			};
			do_ioctl_err(fd, DRM_IOCTL_PANTHOR_BO_SYNC, &sync, EINVAL);
		}

		/* stride > sizeof(op) with a zero tail is accepted. */
		{
			struct {
				struct drm_panthor_bo_sync_op op;
				uint8_t tail[16];
			} padded = {
				.op = {
					.handle = bo.handle,
					.type = DRM_PANTHOR_BO_SYNC_CPU_CACHE_FLUSH,
					.size = PAGE_SIZE,
				},
				.tail = { 0 },
			};
			struct drm_panthor_bo_sync sync = {
				.ops = {
					.count = 1,
					.stride = sizeof(padded),
					.array = to_user_pointer(&padded),
				},
			};
			do_ioctl(fd, DRM_IOCTL_PANTHOR_BO_SYNC, &sync);
		}

		/* stride > sizeof(op) with a nonzero tail is rejected with E2BIG. */
		{
			struct {
				struct drm_panthor_bo_sync_op op;
				uint8_t tail[16];
			} padded = {
				.op = {
					.handle = bo.handle,
					.type = DRM_PANTHOR_BO_SYNC_CPU_CACHE_FLUSH,
					.size = PAGE_SIZE,
				},
				.tail = { 1 },
			};
			struct drm_panthor_bo_sync sync = {
				.ops = {
					.count = 1,
					.stride = sizeof(padded),
					.array = to_user_pointer(&padded),
				},
			};
			do_ioctl_err(fd, DRM_IOCTL_PANTHOR_BO_SYNC, &sync, E2BIG);
		}

		/* count == 0 is a no-op success. */
		{
			struct drm_panthor_bo_sync sync = {
				.ops = {
					.count = 0,
					.stride = sizeof(struct drm_panthor_bo_sync_op),
					.array = 0,
				},
			};
			do_ioctl(fd, DRM_IOCTL_PANTHOR_BO_SYNC, &sync);
		}

		igt_panthor_free_bo(fd, &bo);
	}

	igt_describe("Create a write-back cacheable BO, mmap it, and round-trip BO_SYNC");
	igt_subtest("bo_wb_mmap") {
		struct panthor_bo bo;
		volatile uint8_t *p;

		igt_require(has_wb_mmap(fd));
		igt_require(has_bo_sync(fd));

		igt_panthor_bo_create_mapped(fd, &bo, PAGE_SIZE,
					     DRM_PANTHOR_BO_WB_MMAP, 0);
		igt_assert_neq(bo.handle, 0);
		igt_assert(bo.map);

		/* CPU write/read through the cached mapping. */
		p = bo.map;
		p[0] = 0xa5;
		p[PAGE_SIZE - 1] = 0x5a;
		igt_assert_eq(p[0], 0xa5);
		igt_assert_eq(p[PAGE_SIZE - 1], 0x5a);

		/* FLUSH over the whole cached BO succeeds. */
		{
			struct drm_panthor_bo_sync_op op = {
				.handle = bo.handle,
				.type = DRM_PANTHOR_BO_SYNC_CPU_CACHE_FLUSH,
				.size = PAGE_SIZE,
			};
			igt_assert_eq(do_bo_sync_op(fd, &op), 0);
		}

		/* FLUSH_AND_INVALIDATE over the whole cached BO succeeds. */
		{
			struct drm_panthor_bo_sync_op op = {
				.handle = bo.handle,
				.type = DRM_PANTHOR_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE,
				.size = PAGE_SIZE,
			};
			igt_assert_eq(do_bo_sync_op(fd, &op), 0);
		}

		/*
		 * A partial range succeeds. The driver rounds to cache-line
		 * granularity but accepts any in-bounds range.
		 */
		{
			struct drm_panthor_bo_sync_op op = {
				.handle = bo.handle,
				.type = DRM_PANTHOR_BO_SYNC_CPU_CACHE_FLUSH,
				.offset = 64,
				.size = 128,
			};
			igt_assert_eq(do_bo_sync_op(fd, &op), 0);
		}

		igt_panthor_free_bo(fd, &bo);
	}

	igt_describe("NO_MMAP and WB_MMAP are mutually exclusive at creation time");
	igt_subtest("bo_mmap_flags_exclusive") {
		struct drm_panthor_bo_create args = {
			.size = PAGE_SIZE,
			.flags = DRM_PANTHOR_BO_NO_MMAP | DRM_PANTHOR_BO_WB_MMAP,
		};

		igt_require(has_wb_mmap(fd));

		do_ioctl_err(fd, DRM_IOCTL_PANTHOR_BO_CREATE, &args, EINVAL);
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
