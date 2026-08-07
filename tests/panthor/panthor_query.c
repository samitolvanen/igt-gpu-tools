// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (C) 2025 Collabora Ltd.
// SPDX-FileCopyrightText: Copyright (C) 2026 Google LLC

#include "igt.h"
#include "igt_core.h"
#include "igt_panthor.h"
#include "panthor_drm.h"
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

/* Extra bytes appended to a query buffer to observe the zero fill. */
#define QUERY_TAIL 16

/*
 * Issue a DEV_QUERY and return the raw ioctl result (0 on success, -1 on
 * failure with errno set). On success @out_size receives the size the driver
 * reports back.
 */
static int do_query(int fd, uint32_t type, void *ptr, uint32_t size,
		    uint32_t *out_size)
{
	struct drm_panthor_dev_query query = {
		.type = type,
		.pointer = (uintptr_t)ptr,
		.size = size,
	};
	int ret;

	ret = igt_ioctl(fd, DRM_IOCTL_PANTHOR_DEV_QUERY, &query);
	if (ret == 0 && out_size)
		*out_size = query.size;

	return ret;
}

/* Size the driver reports for a query type when no buffer is passed. */
static uint32_t query_size(int fd, uint32_t type)
{
	uint32_t size = 0;

	igt_assert_eq(do_query(fd, type, NULL, 0, &size), 0);
	igt_assert_neq_u32(size, 0);

	return size;
}

static int set_user_mmio_offset(int fd, uint64_t offset)
{
	struct drm_panthor_set_user_mmio_offset args = { .offset = offset };

	return igt_ioctl(fd, DRM_IOCTL_PANTHOR_SET_USER_MMIO_OFFSET, &args);
}

/*
 * Map the LATEST_FLUSH_ID register page at @offset and read it, so that the
 * driver's fault handler has to back the mapping. Returns true if the driver
 * accepted the offset.
 */
static bool map_flush_id(int fd, uint64_t offset)
{
	size_t size = getpagesize();
	volatile uint32_t *ptr;

	ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, (off_t)offset);
	if (ptr == MAP_FAILED)
		return false;

	(void)*ptr;
	munmap((void *)ptr, size);

	return true;
}

int igt_main() {
	int fd = -1;

	igt_fixture() {
		igt_panthor_skip_on_big_endian();
		fd = drm_open_driver(DRIVER_PANTHOR);
	}

	igt_describe("Query GPU information from ROM.");
	igt_subtest("query") {
		struct drm_panthor_gpu_info gpu = {};

		igt_panthor_query(fd, DRM_PANTHOR_DEV_QUERY_GPU_INFO, &gpu, sizeof(gpu), 0);

		igt_assert_neq(gpu.gpu_id, 0);
	}

	igt_describe("The command stream interface query reports the slot and "
		     "register counts the firmware exposes.");
	igt_subtest("query_csif") {
		struct drm_panthor_csif_info csif = {};

		igt_panthor_query(fd, DRM_PANTHOR_DEV_QUERY_CSIF_INFO, &csif,
				  sizeof(csif), 0);

		igt_assert_neq_u32(csif.csg_slot_count, 0);
		igt_assert_neq_u32(csif.cs_slot_count, 0);
		igt_assert_neq_u32(csif.cs_reg_count, 0);
		igt_assert_neq_u32(csif.scoreboard_slot_count, 0);
		igt_assert_eq_u32(csif.pad, 0);
	}

	igt_describe("The GPU timestamp never goes backwards between two "
		     "queries.");
	igt_subtest("query_timestamp") {
		struct drm_panthor_timestamp_info first = {}, second = {};

		igt_panthor_query(fd, DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO,
				  &first, sizeof(first), 0);
		igt_panthor_query(fd, DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO,
				  &second, sizeof(second), 0);

		igt_assert(second.current_timestamp >= first.current_timestamp);
	}

	igt_describe("The timestamp query returns only the sources its flags "
		     "ask for, and rejects flags it cannot honor.");
	igt_subtest("query_timestamp_flags") {
		struct drm_panthor_timestamp_info info;

		/*
		 * Flags are a later addition to this query. A driver that
		 * predates them reports a shorter structure and would reject
		 * the flags field as a nonzero tail; one that reports the full
		 * size but ignores the field returns no CPU timestamp.
		 */
		igt_require_f(query_size(fd, DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO) >=
			      sizeof(info),
			      "timestamp query flags unsupported\n");

		info = (struct drm_panthor_timestamp_info){
			.flags = DRM_PANTHOR_TIMESTAMP_GPU |
				 DRM_PANTHOR_TIMESTAMP_CPU_MONOTONIC,
		};
		igt_panthor_query(fd, DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO,
				  &info, sizeof(info), 0);
		igt_require_f(info.cpu_timestamp_sec != 0,
			      "timestamp query flags unsupported\n");
		igt_assert_neq_u64(info.current_timestamp, 0);

		/* Sources that were not asked for come back as zero. */
		info = (struct drm_panthor_timestamp_info){
			.flags = DRM_PANTHOR_TIMESTAMP_GPU,
		};
		igt_panthor_query(fd, DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO,
				  &info, sizeof(info), 0);
		igt_assert_eq_u64(info.cpu_timestamp_sec, 0);
		igt_assert_eq_u64(info.cpu_timestamp_nsec, 0);
		igt_assert_eq_u64(info.timestamp_offset, 0);
		igt_assert_eq_u64(info.timestamp_frequency, 0);
		igt_assert_eq_u64(info.cycle_count, 0);

		info = (struct drm_panthor_timestamp_info){
			.flags = DRM_PANTHOR_TIMESTAMP_FREQ,
		};
		igt_panthor_query(fd, DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO,
				  &info, sizeof(info), 0);
		igt_assert_eq_u64(info.current_timestamp, 0);

		/*
		 * The CPU clock names are values of one enumerated field, not
		 * independent bits, so combining two of them names no clock.
		 */
		info = (struct drm_panthor_timestamp_info){
			.flags = DRM_PANTHOR_TIMESTAMP_CPU_MONOTONIC |
				 DRM_PANTHOR_TIMESTAMP_CPU_MONOTONIC_RAW,
		};
		igt_panthor_query(fd, DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO,
				  &info, sizeof(info), EINVAL);

		/* An undefined flag bit is rejected. */
		info = (struct drm_panthor_timestamp_info){
			.flags = 1u << 31,
		};
		igt_panthor_query(fd, DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO,
				  &info, sizeof(info), EINVAL);
	}

	igt_describe("The group priorities query reports a mask that always "
		     "contains the priorities every client may use.");
	igt_subtest("query_group_priorities") {
		struct drm_panthor_group_priorities_info info = {};

		igt_panthor_query(fd,
				  DRM_PANTHOR_DEV_QUERY_GROUP_PRIORITIES_INFO,
				  &info, sizeof(info), 0);

		/* Medium priority and below need no privileges. */
		igt_assert(info.allowed_mask & (1 << PANTHOR_GROUP_PRIORITY_LOW));
		igt_assert(info.allowed_mask & (1 << PANTHOR_GROUP_PRIORITY_MEDIUM));

		/* Realtime is the highest priority the uAPI defines. */
		igt_assert_eq_u32(info.allowed_mask &
				  ~((1u << (PANTHOR_GROUP_PRIORITY_REALTIME + 1)) - 1),
				  0);

		igt_assert_eq_u32(info.pad[0] | info.pad[1] | info.pad[2], 0);
	}

	igt_describe("Every query type reports its size when no buffer is "
		     "passed, rejects a buffer too small to hold the mandatory "
		     "fields, and zero fills the tail of a larger one.");
	igt_subtest("query_size_negotiation") {
		static const uint32_t types[] = {
			DRM_PANTHOR_DEV_QUERY_GPU_INFO,
			DRM_PANTHOR_DEV_QUERY_CSIF_INFO,
			DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO,
			DRM_PANTHOR_DEV_QUERY_GROUP_PRIORITIES_INFO,
		};
		/*
		 * The timestamp query is the one that also reads its buffer, so
		 * it is left out of the tail check: a poisoned tail is not a
		 * valid input there.
		 */
		static const uint32_t output_only[] = {
			DRM_PANTHOR_DEV_QUERY_GPU_INFO,
			DRM_PANTHOR_DEV_QUERY_CSIF_INFO,
			DRM_PANTHOR_DEV_QUERY_GROUP_PRIORITIES_INFO,
		};

		/* Large enough to serve as a buffer for any of the types. */
		uint8_t buf[sizeof(struct drm_panthor_gpu_info)] = {};
		/* Fields GPU_INFO carried when it was added to the uAPI. */
		uint32_t gpu_info_min =
			offsetof(struct drm_panthor_gpu_info, tiler_present) +
			sizeof(((struct drm_panthor_gpu_info *)NULL)->tiler_present);

		for (unsigned int i = 0; i < ARRAY_SIZE(types); i++) {
			query_size(fd, types[i]);

			igt_assert_eq(do_query(fd, types[i], buf, 0, NULL), -1);
			igt_assert_eq(errno, EINVAL);
		}

		/* Userspace built against an older header still gets served. */
		igt_assert_eq(do_query(fd, DRM_PANTHOR_DEV_QUERY_GPU_INFO, buf,
				       gpu_info_min, NULL), 0);
		igt_assert_eq(do_query(fd, DRM_PANTHOR_DEV_QUERY_GPU_INFO, buf,
				       gpu_info_min - 1, NULL), -1);
		igt_assert_eq(errno, EINVAL);

		for (unsigned int i = 0; i < ARRAY_SIZE(output_only); i++) {
			uint32_t size = query_size(fd, output_only[i]);
			uint8_t *poisoned = malloc(size + QUERY_TAIL);

			igt_assert(poisoned);
			memset(poisoned, 0xff, size + QUERY_TAIL);

			igt_assert_eq(do_query(fd, output_only[i], poisoned,
					       size + QUERY_TAIL, NULL), 0);
			for (uint32_t j = size; j < size + QUERY_TAIL; j++)
				igt_assert_eq_u32(poisoned[j], 0);

			free(poisoned);
		}

		/* An unknown query type is rejected either way. */
		igt_assert_eq(do_query(fd, 0xdeadbeef, NULL, 0, NULL), -1);
		igt_assert_eq(errno, EINVAL);
		{
			struct drm_panthor_gpu_info gpu;

			igt_assert_eq(do_query(fd, 0xdeadbeef, &gpu,
					       sizeof(gpu), NULL), -1);
			igt_assert_eq(errno, EINVAL);
		}
	}

	igt_describe("SET_USER_MMIO_OFFSET moves the user MMIO window of one "
		     "file descriptor and leaves buffer object mappings alone.");
	igt_subtest("set_user_mmio_offset") {
		int local_fd, ret, saved_errno;
		struct panthor_bo bo = {};

		/* A 32-bit process cannot reach the 64-bit window at all. */
		igt_require(DRM_PANTHOR_USER_MMIO_OFFSET ==
			    DRM_PANTHOR_USER_MMIO_OFFSET_64BIT);

		local_fd = drm_reopen_driver(fd);

		ret = set_user_mmio_offset(local_fd,
					   DRM_PANTHOR_USER_MMIO_OFFSET_64BIT);
		saved_errno = errno;
		if (ret) {
			close(local_fd);
			igt_skip("SET_USER_MMIO_OFFSET unsupported (errno %d)\n",
				 saved_errno);
		}

		/* Only the two documented windows are accepted. */
		igt_assert_eq(set_user_mmio_offset(local_fd, 0), -1);
		igt_assert_eq(errno, EINVAL);
		igt_assert_eq(set_user_mmio_offset(local_fd, getpagesize()), -1);
		igt_assert_eq(errno, EINVAL);
		igt_assert_eq(set_user_mmio_offset(local_fd,
						   DRM_PANTHOR_USER_MMIO_OFFSET_32BIT +
						   getpagesize()), -1);
		igt_assert_eq(errno, EINVAL);
		igt_assert_eq(set_user_mmio_offset(local_fd,
						   DRM_PANTHOR_USER_MMIO_OFFSET_64BIT - 1), -1);
		igt_assert_eq(errno, EINVAL);

		igt_assert(map_flush_id(local_fd,
					DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET));
		igt_assert(!map_flush_id(local_fd,
					 DRM_PANTHOR_USER_MMIO_OFFSET_32BIT));

		/* Selecting the other window swaps which offset is accepted. */
		igt_assert_eq(set_user_mmio_offset(local_fd,
						   DRM_PANTHOR_USER_MMIO_OFFSET_32BIT), 0);
		igt_assert(map_flush_id(local_fd,
					DRM_PANTHOR_USER_MMIO_OFFSET_32BIT));
		igt_assert(!map_flush_id(local_fd,
					 DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET));

		/* Buffer object offsets sit below both windows and still map. */
		igt_panthor_bo_create_mapped(local_fd, &bo, getpagesize(), 0, 0);
		igt_assert(bo.map);
		igt_panthor_free_bo(local_fd, &bo);

		close(local_fd);
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
