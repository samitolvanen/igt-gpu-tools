// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (C) 2026 Google LLC

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "igt.h"
#include "igt_aux.h"
#include "igt_panthor.h"
#include "igt_pm.h"
#include "igt_syncobj.h"
#include "panthor_drm.h"

/*
 * Power-management tests for the Tyr/Panthor driver, modelled on the i915 and
 * xe runtime-PM suites. They drive the GPU with a small counted-loop workload
 * (verified by reading back the counter the GPU wrote) and inspect the generic
 * runtime-PM sysfs surface via the igt_pm helpers.
 *
 * The runtime-PM subtests require runtime PM to be available for the device
 * (CONFIG_PM and a platform that can gate the GPU); igt_setup_runtime_pm()
 * forces the autosuspend delay to 0 so an idle GPU suspends promptly and the
 * resume races are exercised hard.
 *
 * The system-sleep subtests suspend the WHOLE machine and rely on the RTC
 * wake alarm to resume it. A board whose RTC cannot wake it from the target
 * state hangs with no way back but a hard reset, and rtcwake's dry-run check
 * does not verify that wake actually works. They are therefore opt-in: set
 * PANTHOR_PM_ALLOW_SYSTEM_SUSPEND=1 to run them, only after confirming that
 * e.g. "rtcwake -s 5 -m freeze" really wakes your board.
 */

#define INITIAL_VA	0x1000000

/* Byte offset inside the scratch BO for the GPU-visible loop counter. */
#define COUNTER_OFFSET	2064

#define SEC_NS		1000000000ULL

/* A short job completes in milliseconds even at the boot clock. */
#define SHORT_LOOP_N	4096

/* GPU core masks, queried once in the top fixture. */
static uint64_t g_shader_present;
static uint64_t g_tiler_present;

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
 * times, then completes. CSF branch offsets are relative to the next
 * instruction, so the encoded value is (target - branch - 1) in instruction
 * units. Condition EQ ("branch if source register == 0") emulates an
 * unconditional back-branch by testing a register pre-loaded with 0.
 *
 * Returns the number of 64-bit instruction words emitted.
 */
static int emit_counted_loop(uint64_t *instrs, uint64_t counter_addr, uint32_t n)
{
	int k = 0;
	int body, br_exit, br_back, exit;

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

static struct drm_panthor_sync_op signal_op(uint32_t syncobj)
{
	return (struct drm_panthor_sync_op){
		.flags = DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ |
			 DRM_PANTHOR_SYNC_OP_SIGNAL,
		.handle = syncobj,
	};
}

/*
 * A counted-loop workload: its handles and the mapped scratch BO. Submit it
 * with submit_workload(), wait on .syncobj, read the counter back at
 * COUNTER_OFFSET to confirm execution, then tear it down.
 */
struct workload {
	uint32_t vm_id;
	uint32_t group_handle;
	uint32_t syncobj;
	struct panthor_bo bo;
};

static void submit_workload(int fd, struct workload *w, uint32_t n)
{
	struct drm_panthor_queue_create queue = {
		.priority = 0, .ringbuf_size = 4096,
	};
	struct drm_panthor_group_create cfg;
	struct drm_panthor_sync_op sync;
	struct drm_panthor_queue_submit submit;
	struct drm_panthor_group_submit group_submit;
	uint64_t instrs[16];
	int ninstrs;

	igt_panthor_vm_create(fd, &w->vm_id, 0);
	w->syncobj = syncobj_create(fd, 0);
	igt_panthor_bo_create_mapped(fd, &w->bo, 4096, 0, 0);

	*(volatile uint32_t *)((uint8_t *)w->bo.map + COUNTER_OFFSET) = 0;

	ninstrs = emit_counted_loop(instrs, INITIAL_VA + COUNTER_OFFSET, n);
	memcpy(w->bo.map, instrs, ninstrs * sizeof(instrs[0]));

	igt_panthor_vm_bind(fd, w->vm_id, w->bo.handle, INITIAL_VA, w->bo.size,
			    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP |
			    DRM_PANTHOR_VM_BIND_OP_MAP_UNCACHED, 0);

	cfg = make_group_cfg(&queue, 1, PANTHOR_GROUP_PRIORITY_LOW, w->vm_id);
	igt_panthor_group_create(fd, &cfg, 0);
	w->group_handle = cfg.group_handle;

	sync = signal_op(w->syncobj);
	submit = (struct drm_panthor_queue_submit){
		.queue_index = 0,
		.stream_addr = INITIAL_VA,
		.stream_size = ninstrs * sizeof(instrs[0]),
		.syncs = DRM_PANTHOR_OBJ_ARRAY(1, &sync),
	};
	group_submit = (struct drm_panthor_group_submit){
		.group_handle = w->group_handle,
		.queue_submits = DRM_PANTHOR_OBJ_ARRAY(1, &submit),
	};
	igt_panthor_group_submit(fd, &group_submit, 0);
}

static uint32_t workload_counter(struct workload *w)
{
	return *(volatile uint32_t *)((uint8_t *)w->bo.map + COUNTER_OFFSET);
}

static void teardown_workload(int fd, struct workload *w)
{
	igt_panthor_group_destroy(fd, w->group_handle, 0);
	syncobj_destroy(fd, w->syncobj);
	igt_panthor_free_bo(fd, &w->bo);
	igt_panthor_vm_destroy(fd, w->vm_id, 0);
}

/*
 * Run one short job to completion and assert the GPU executed it: the loop
 * writes its iteration count to the counter, so a correct run leaves exactly n
 * there. Proves the GPU resumed (when suspended) and produced correct results.
 */
static void run_verified_job(int fd, uint32_t n)
{
	struct workload w = {};

	submit_workload(fd, &w, n);
	igt_assert_f(wait_done(fd, w.syncobj, 60 * SEC_NS),
		     "workload did not complete\n");
	igt_assert_eq_u32(workload_counter(&w), n);
	teardown_workload(fd, &w);
}

static const struct {
	const char *name;
	enum igt_suspend_state state;
} suspend_modes[] = {
	{ "s2idle", SUSPEND_STATE_FREEZE },
	{ "mem", SUSPEND_STATE_MEM },
};

int igt_main()
{
	int fd = -1;
	bool has_rpm = false;

	igt_fixture() {
		struct drm_panthor_gpu_info gpu_info = {};

		igt_panthor_skip_on_big_endian();
		fd = drm_open_driver(DRIVER_PANTHOR);

		igt_panthor_query(fd, DRM_PANTHOR_DEV_QUERY_GPU_INFO,
				  &gpu_info, sizeof(gpu_info), 0);
		g_shader_present = gpu_info.shader_present;
		g_tiler_present = gpu_info.tiler_present;

		/* Forces autosuspend_delay_ms to 0; restored by an exit handler. */
		has_rpm = igt_setup_runtime_pm(fd);
	}

	igt_describe("The GPU runtime-suspends once it goes idle.");
	igt_subtest("rpm-idle-suspend") {
		igt_require_f(has_rpm, "runtime PM is not available for the GPU\n");
		igt_assert_f(igt_wait_for_pm_status(IGT_RUNTIME_PM_STATUS_SUSPENDED),
			     "GPU did not runtime-suspend when idle\n");
	}

	igt_describe("A submission resumes a suspended GPU, runs correctly, and "
		     "the GPU re-suspends once idle again.");
	igt_subtest("rpm-resume-on-exec") {
		igt_require_f(has_rpm, "runtime PM is not available for the GPU\n");
		igt_assert_f(igt_wait_for_pm_status(IGT_RUNTIME_PM_STATUS_SUSPENDED),
			     "GPU did not reach a suspended baseline\n");

		run_verified_job(fd, SHORT_LOOP_N);

		igt_assert_f(igt_wait_for_pm_status(IGT_RUNTIME_PM_STATUS_SUSPENDED),
			     "GPU did not re-suspend after the job completed\n");
	}

	igt_describe("Repeated suspend/resume/suspend cycles run correctly, "
		     "exercising the resume-in-progress and lost-kick paths.");
	igt_subtest("rpm-multiple-execs") {
		igt_require_f(has_rpm, "runtime PM is not available for the GPU\n");

		for (int i = 0; i < 8; i++) {
			igt_assert_f(igt_wait_for_pm_status(
					     IGT_RUNTIME_PM_STATUS_SUSPENDED),
				     "GPU did not suspend before cycle %d\n", i);
			run_verified_job(fd, SHORT_LOOP_N);
		}
	}

	for (int i = 0; i < (int)ARRAY_SIZE(suspend_modes); i++) {
		igt_describe("The GPU survives a system suspend/resume cycle and "
			     "runs work correctly before and after. Opt-in: a "
			     "board whose RTC cannot wake it will hang here.");
		igt_subtest_f("system-%s", suspend_modes[i].name) {
			igt_require_f(getenv("PANTHOR_PM_ALLOW_SYSTEM_SUSPEND"),
				      "system suspend can hang a board whose RTC "
				      "cannot wake it from this state, and rtcwake's "
				      "dry-run does not detect that. Confirm a real "
				      "wake works (e.g. 'rtcwake -s 5 -m freeze') and "
				      "set PANTHOR_PM_ALLOW_SYSTEM_SUSPEND=1 to run "
				      "this subtest.\n");

			run_verified_job(fd, SHORT_LOOP_N);

			igt_system_suspend_autoresume(suspend_modes[i].state,
						      SUSPEND_TEST_NONE);

			run_verified_job(fd, SHORT_LOOP_N);
		}
	}

	igt_fixture() {
		if (has_rpm)
			igt_restore_runtime_pm();
		drm_close_driver(fd);
	}
}
