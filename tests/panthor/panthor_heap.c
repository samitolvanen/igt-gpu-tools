// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (C) 2026 Google LLC

#include <stdint.h>
#include <unistd.h>

#include "igt.h"
#include "igt_panthor.h"
#include "panthor_drm.h"

/*
 * Tiler heap tests.
 *
 * A heap context belongs to a VM and owns a chain of chunks that the firmware
 * grows on demand while a tiler job runs. Nothing here drives the tiler, so
 * the coverage is the argument checking TILER_HEAP_CREATE does and the
 * create/destroy lifecycle of a heap and of the per-VM pool behind it.
 */

/* Chunk size range the uAPI documents. */
#define CHUNK_SIZE_MIN	SZ_128K
#define CHUNK_SIZE_MAX	SZ_8M

/*
 * Create a tiler heap and return the raw ioctl result (0 on success, -1 on
 * failure with errno set). On success @handle receives the heap handle.
 */
static int do_heap_create(int fd, uint32_t vm_id, uint32_t initial_chunk_count,
			  uint32_t chunk_size, uint32_t max_chunks,
			  uint32_t target_in_flight, uint32_t *handle)
{
	struct drm_panthor_tiler_heap_create args = {
		.vm_id = vm_id,
		.initial_chunk_count = initial_chunk_count,
		.chunk_size = chunk_size,
		.max_chunks = max_chunks,
		.target_in_flight = target_in_flight,
	};
	int ret;

	ret = igt_ioctl(fd, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &args);
	if (ret)
		return ret;

	igt_assert_eq_u32(args.handle >> 16, vm_id);
	igt_assert_neq_u64(args.tiler_heap_ctx_gpu_va, 0);
	igt_assert_neq_u64(args.first_heap_chunk_gpu_va, 0);

	if (handle)
		*handle = args.handle;

	return 0;
}

static int do_heap_destroy(int fd, uint32_t handle)
{
	struct drm_panthor_tiler_heap_destroy args = { .handle = handle };

	return igt_ioctl(fd, DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY, &args);
}

/*
 * Create and destroy one minimal heap, and skip the calling subtest if the
 * driver has no tiler heap ioctls at all. Self-contained, so that the skip
 * leaves nothing allocated.
 */
static void require_tiler_heap(int fd)
{
	uint32_t vm_id, handle = 0;
	int ret, saved_errno;

	igt_panthor_vm_create(fd, &vm_id, 0);

	ret = do_heap_create(fd, vm_id, 1, CHUNK_SIZE_MIN, 1, 1, &handle);
	saved_errno = errno;
	if (ret == 0)
		igt_assert_eq(do_heap_destroy(fd, handle), 0);

	igt_panthor_vm_destroy(fd, vm_id, 0);

	igt_require_f(ret == 0, "tiler heap ioctls unsupported (errno %d)\n",
		      saved_errno);
}

int igt_main()
{
	int fd = -1;

	igt_fixture() {
		igt_panthor_skip_on_big_endian();
		fd = drm_open_driver(DRIVER_PANTHOR);
	}

	igt_describe("Accept the documented tiler heap chunk sizes and chunk "
		     "counts, and reject the values on either side of them.");
	igt_subtest("heap_create_validation") {
		uint32_t page = getpagesize();
		uint32_t vm_id, handle;

		require_tiler_heap(fd);

		igt_panthor_vm_create(fd, &vm_id, 0);

		/* target_in_flight has no range check at creation time. */
		igt_assert_eq(do_heap_create(fd, vm_id, 1, CHUNK_SIZE_MIN, 1, 0,
					     &handle), 0);
		igt_assert_eq(do_heap_destroy(fd, handle), 0);

		igt_assert_eq(do_heap_create(fd, vm_id, 1, CHUNK_SIZE_MAX, 1, 1,
					     &handle), 0);
		igt_assert_eq(do_heap_destroy(fd, handle), 0);

		/* Only page alignment is required, not a power of two. */
		igt_assert_eq(do_heap_create(fd, vm_id, 1, CHUNK_SIZE_MIN + page,
					     1, 1, &handle), 0);
		igt_assert_eq(do_heap_destroy(fd, handle), 0);

		igt_assert_eq(do_heap_create(fd, vm_id, 1, CHUNK_SIZE_MIN - page,
					     1, 1, NULL), -1);
		igt_assert_eq(errno, EINVAL);

		igt_assert_eq(do_heap_create(fd, vm_id, 1, CHUNK_SIZE_MAX + page,
					     1, 1, NULL), -1);
		igt_assert_eq(errno, EINVAL);

		igt_assert_eq(do_heap_create(fd, vm_id, 1, CHUNK_SIZE_MIN + 1, 1,
					     1, NULL), -1);
		igt_assert_eq(errno, EINVAL);

		igt_assert_eq(do_heap_create(fd, vm_id, 1, 0, 1, 1, NULL), -1);
		igt_assert_eq(errno, EINVAL);

		igt_assert_eq(do_heap_create(fd, vm_id, 0, CHUNK_SIZE_MIN, 1, 1,
					     NULL), -1);
		igt_assert_eq(errno, EINVAL);

		igt_assert_eq(do_heap_create(fd, vm_id, 2, CHUNK_SIZE_MIN, 1, 1,
					     NULL), -1);
		igt_assert_eq(errno, EINVAL);

		/* A zero maximum leaves no room for the initial chunk. */
		igt_assert_eq(do_heap_create(fd, vm_id, 1, CHUNK_SIZE_MIN, 0, 1,
					     NULL), -1);
		igt_assert_eq(errno, EINVAL);

		igt_assert_eq(do_heap_create(fd, vm_id, 4, CHUNK_SIZE_MIN, 4, 1,
					     &handle), 0);
		igt_assert_eq(do_heap_destroy(fd, handle), 0);

		igt_assert_eq(do_heap_create(fd, 0xdead, 1, CHUNK_SIZE_MIN, 1, 1,
					     NULL), -1);
		igt_assert_eq(errno, EINVAL);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Create and destroy tiler heaps while their VM stays "
		     "alive, and reject destroy requests naming a heap or a "
		     "pool that is not there.");
	igt_subtest("heap_create_destroy") {
		uint32_t vm_id, poolless_vm, handles[4];

		require_tiler_heap(fd);

		igt_panthor_vm_create(fd, &vm_id, 0);

		for (int i = 0; i < 8; i++) {
			uint32_t handle;

			igt_assert_eq(do_heap_create(fd, vm_id, 1,
						     CHUNK_SIZE_MIN, 4, 1,
						     &handle), 0);
			igt_assert_eq(do_heap_destroy(fd, handle), 0);
		}

		for (int i = 0; i < 4; i++)
			igt_assert_eq(do_heap_create(fd, vm_id, 1,
						     CHUNK_SIZE_MIN, 4, 1,
						     &handles[i]), 0);

		for (int i = 0; i < 4; i++)
			igt_assert_eq(do_heap_destroy(fd, handles[i]), 0);

		igt_assert_eq(do_heap_destroy(fd, handles[0]), -1);
		igt_assert_eq(errno, EINVAL);

		igt_assert_eq(do_heap_destroy(fd, (vm_id << 16) | 0x7fff), -1);
		igt_assert_eq(errno, EINVAL);

		igt_assert_eq(do_heap_destroy(fd, 0xdeadu << 16), -1);
		igt_assert_eq(errno, EINVAL);

		{
			struct drm_panthor_tiler_heap_destroy args = {
				.handle = handles[0],
				.pad = 1,
			};
			do_ioctl_err(fd, DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY,
				     &args, EINVAL);
		}

		/*
		 * A VM that never created a heap has no pool to look the handle
		 * up in, which the ioctl reports as ENOENT rather than EINVAL.
		 */
		igt_panthor_vm_create(fd, &poolless_vm, 0);
		igt_assert_eq(do_heap_destroy(fd, poolless_vm << 16), -1);
		igt_assert_eq(errno, ENOENT);
		igt_panthor_vm_destroy(fd, poolless_vm, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
