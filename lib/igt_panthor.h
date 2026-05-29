/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: Copyright (C) 2025 Collabora Ltd. */

#ifndef IGT_PANTHOR_H
#define IGT_PANTHOR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "panthor_drm.h"

struct panthor_bo {
	int handle;
	uint64_t offset;
	uint64_t size;
	void *map;
};

void igt_panthor_query(int fd, int32_t type, void *data, size_t size, int err);
void igt_panthor_vm_create(int fd, uint32_t *vm_id, int err);
void igt_panthor_vm_destroy(int fd, uint32_t vm_id, int err);
void igt_panthor_vm_bind_offset(int fd, uint32_t vm_id, uint32_t bo_handle, uint64_t va,
				uint64_t size, uint64_t ofsfet, uint32_t flags, int err);
void igt_panthor_bo_create(int fd, struct panthor_bo *bo, uint64_t size, uint32_t flags, int err);
uint64_t igt_panthor_bo_mmap_offset(int fd, uint32_t handle, int err);
void igt_panthor_free_bo(int fd, struct panthor_bo *bo);
void igt_panthor_bo_create_mapped(int fd, struct panthor_bo *bo, uint64_t size,
				  uint32_t flags, int err);
void *igt_panthor_mmap_bo(int fd, uint32_t handle, uint64_t size,
			  unsigned int prot, uint64_t offset);
void igt_panthor_group_create(int fd, struct drm_panthor_group_create *group_create, int err);
void igt_panthor_group_destroy(int fd, uint32_t group_handle, int err);
void igt_panthor_group_submit(int fd, struct drm_panthor_group_submit *group_submit, int err);
uint32_t igt_panthor_group_create_simple(int fd, uint32_t vm_id, int err);
void igt_panthor_group_submit_simple(int fd, uint32_t group_handle,
				     uint32_t queue_index, uint64_t stream_addr,
				     uint32_t stream_size, uint32_t syncobj_handle,
				     int err);
uint64_t igt_panthor_get_first_core(uint64_t cores_present);

static inline void igt_panthor_vm_bind(int fd, uint32_t vm_id, uint32_t bo_handle,
				       uint64_t va, uint64_t size, uint32_t flags, int err)
{
	igt_panthor_vm_bind_offset(fd, vm_id, bo_handle, va, size, 0, flags, err);
}

enum cs_opcode {
	CS_OPCODE_NOP = 0,
	CS_OPCODE_MOVE48 = 1,
	CS_OPCODE_MOVE32 = 2,
	CS_OPCODE_WAIT = 3,
	CS_OPCODE_ADD_IMM32 = 16,
	CS_OPCODE_LOAD_MULTIPLE = 20,
	CS_OPCODE_STM = 21,
	CS_OPCODE_BRANCH = 22,
	CS_OPCODE_FLUSH_CACHE = 36,
	CS_OPCODE_SYNC_SET32 = 38,
	CS_OPCODE_SYNC_WAIT32 = 39,
};

enum cs_flush_mode {
	CS_FLUSH_MODE_NONE = 0,
	CS_FLUSH_MODE_CLEAN = 1,
	CS_FLUSH_MODE_INVALIDATE = 2,
	CS_FLUSH_MODE_CLEAN_AND_INVALIDATE = 3,
};

/* Condition codes for BRANCH and SYNC_WAIT operations. */
enum cs_condition {
	CS_CONDITION_GT = 1,
	CS_CONDITION_EQ = 2,
};

/* There's no plan to support big endian in the UMD, so keep
 * things simple and don't bother supporting it.
 */
void igt_panthor_skip_on_big_endian(void);

struct cs_instr {
	union {
		struct {
			uint64_t data: 56;
			uint64_t opcode: 8;
		} any;
		struct {
			uint64_t unused: 56;
			uint64_t opcode: 8;
		} nop;
		struct {
			uint64_t immediate: 48;
			uint64_t dest: 8;
			uint64_t opcode: 8;
		} move48;
		struct {
			uint64_t immediate: 32;
			uint64_t unused: 16;
			uint64_t dest: 8;
			uint64_t opcode: 8;
		} move32;
		struct {
			uint64_t unused0: 16;
			uint64_t wait_mask: 16;
			uint64_t progress_inc: 1;
			uint64_t unused1: 23;
			uint64_t opcode: 8;
		} wait;
		struct {
			uint64_t offset: 16;
			uint64_t mask: 16;
			uint64_t unused: 8;
			uint64_t address: 8;
			uint64_t src: 8;
			uint64_t opcode: 8;
		} stm;
		struct {
			uint64_t l2_mode: 4;
			uint64_t lsc_mode: 4;
			uint64_t other_mode: 4;
			uint64_t unused0: 4;
			uint64_t wait_mask: 16;
			uint64_t unused1: 8;
			uint64_t flush_id: 8;
			uint64_t signal_slot: 4;
			uint64_t unused2: 4;
			uint64_t opcode: 8;
		} flush;
		struct {
			uint64_t immediate: 32;
			uint64_t unused0: 8;
			uint64_t src: 8;
			uint64_t dest: 8;
			uint64_t opcode: 8;
		} add_imm32;
		struct {
			uint64_t offset: 16;
			uint64_t bitmap: 24;
			uint64_t address: 8;
			uint64_t sw: 8;
			uint64_t opcode: 8;
		} load_multiple;
		struct {
			uint64_t offset: 16;
			uint64_t unused0: 12;
			uint64_t condition: 4;
			uint64_t unused1: 8;
			uint64_t src: 8;
			uint64_t unused2: 8;
			uint64_t opcode: 8;
		} branch;
		struct {
			uint64_t unused0: 32;
			uint64_t val: 8;
			uint64_t address: 8;
			uint64_t unused1: 8;
			uint64_t opcode: 8;
		} sync_set32;
		struct {
			uint64_t unused0: 28;
			uint64_t condition: 4;
			uint64_t val: 8;
			uint64_t address: 8;
			uint64_t unused1: 8;
			uint64_t opcode: 8;
		} sync_wait32;
		uint64_t raw;
	};
};

static inline uint64_t
cs_nop(void)
{
	struct cs_instr instr = {
		.nop = {
			.opcode = CS_OPCODE_NOP,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_mov48(uint8_t dst, uint64_t imm)
{
	struct cs_instr instr = {
		.move48 = {
			.opcode = CS_OPCODE_MOVE48,
			.dest = dst,
			.immediate = (uint64_t)imm & 0xffffffffffffull,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_mov32(uint8_t dst, uint32_t imm)
{
	struct cs_instr instr = {
		.move32 = {
			.opcode = CS_OPCODE_MOVE32,
			.dest = dst,
			.immediate = (uint32_t)imm,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_wait(uint16_t wait_mask, bool progress_inc)
{
	struct cs_instr instr = {
		.wait = {
			.opcode = CS_OPCODE_WAIT,
			.wait_mask = wait_mask,
			.progress_inc = progress_inc,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_stm(uint8_t address, uint8_t src, uint16_t mask, int16_t offset)
{
	struct cs_instr instr = {
		.stm = {
			.opcode = CS_OPCODE_STM,
			.offset = (uint16_t)offset,
			.mask = mask,
			.src = src,
			.address = address,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_stm32(uint8_t address, uint8_t src, int16_t offset)
{
	return cs_stm(address, src, 0x1, offset);
}

static inline uint64_t
cs_stm64(uint8_t address, uint8_t src, int16_t offset)
{
	return cs_stm(address, src, 0x3, offset);
}

static inline uint64_t
cs_flush(enum cs_flush_mode l2_mode,
	 enum cs_flush_mode lsc_mode,
	 enum cs_flush_mode other_mode,
	 uint16_t wait_mask,
	 uint8_t flush_id,
	 uint8_t signal_slot)
{
	struct cs_instr instr = {
		.flush = {
			.l2_mode = l2_mode,
			.lsc_mode = lsc_mode,
			.other_mode = other_mode,
			.wait_mask = wait_mask,
			.flush_id = flush_id,
			.signal_slot = signal_slot,
			.opcode = CS_OPCODE_FLUSH_CACHE,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_add_imm32(uint8_t dst, uint8_t src, uint32_t imm)
{
	struct cs_instr instr = {
		.add_imm32 = {
			.opcode = CS_OPCODE_ADD_IMM32,
			.dest = dst,
			.src = src,
			.immediate = imm,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_load_multiple(uint8_t address, uint8_t sw, uint32_t bitmap, int16_t offset)
{
	struct cs_instr instr = {
		.load_multiple = {
			.opcode = CS_OPCODE_LOAD_MULTIPLE,
			.sw = sw,
			.address = address,
			.bitmap = bitmap,
			.offset = (uint16_t)offset,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_branch(uint8_t src, enum cs_condition condition, int16_t offset)
{
	struct cs_instr instr = {
		.branch = {
			.opcode = CS_OPCODE_BRANCH,
			.src = src,
			.condition = condition,
			.offset = (uint16_t)offset,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_sync_set32(uint8_t address, uint8_t val)
{
	struct cs_instr instr = {
		.sync_set32 = {
			.opcode = CS_OPCODE_SYNC_SET32,
			.address = address,
			.val = val,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_sync_wait32(uint8_t address, uint8_t val, enum cs_condition condition)
{
	struct cs_instr instr = {
		.sync_wait32 = {
			.opcode = CS_OPCODE_SYNC_WAIT32,
			.address = address,
			.val = val,
			.condition = condition,
		},
	};

	return instr.raw;
}

#endif /* IGT_PANTHOR_H */
