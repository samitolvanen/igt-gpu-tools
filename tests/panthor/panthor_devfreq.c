// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (C) 2026 Google LLC

#include <fcntl.h>
#include <glob.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "igt.h"
#include "igt_panthor.h"
#include "igt_syncobj.h"
#include "panthor_drm.h"

/*
 * Behaviour tests for the Tyr/Panthor devfreq integration. Drives the GPU
 * with a sustained compute
 * workload and inspects the per-device /sys/.../devfreq surface to verify
 * cur_freq ramps up under load and back down when idle, and smoke-checks
 * governor selection and trans_stat presence.
 *
 * devfreq is optional: every subtest requires the devfreq sysfs directory to
 * exist (CONFIG_PM_DEVFREQ and a devfreq instance for the GPU). The ramp tests
 * additionally require more than one operating point, so the governor has
 * something to scale.
 */

#define INITIAL_VA	0x1000000

/* Byte offset inside the scratch BO for the visible loop counter. */
#define COUNTER_OFFSET	2064

/* Relative wait budget in nanoseconds. */
#define SEC_NS		1000000000ULL

/*
 * Iteration count for the workload. ~5 GPU instructions per iteration; at the
 * Mali-G610 boot frequency (~198MHz) this lands comfortably in the multi-second
 * range, giving the simple_ondemand governor (default 100ms polling) ample
 * opportunity to ramp up before the job completes.
 */
#define DEVFREQ_LOOP_N	5000000

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

/*
 * Resolve the GPU's devfreq sysfs directory from the open render-node fd.
 *
 * Returns a malloc'd path like:
 *   /sys/dev/char/MAJOR:MINOR/device/devfreq/<name>
 * or NULL if no devfreq instance exists for the device. The devfreq entry is
 * named after the parent platform device on newer kernels, so glob the
 * directory and take the first (only) match. Caller must free().
 */
static char *find_devfreq_sysfs(int fd)
{
	char pat[256];
	struct stat st;
	glob_t gl;
	char *path;

	if (fstat(fd, &st) < 0)
		return NULL;

	snprintf(pat, sizeof(pat), "/sys/dev/char/%u:%u/device/devfreq/*",
		 major(st.st_rdev), minor(st.st_rdev));

	if (glob(pat, 0, NULL, &gl) != 0)
		return NULL;

	path = (gl.gl_pathc > 0) ? strdup(gl.gl_pathv[0]) : NULL;
	globfree(&gl);

	return path;
}

/*
 * Read a sysfs attribute into a freshly-allocated string, stripping trailing
 * whitespace. Returns NULL on error. Caller must free().
 */
static char *read_sysfs(const char *dir, const char *file)
{
	char path[512];
	char buf[4096];
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return NULL;

	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return NULL;

	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ' ||
			 buf[n - 1] == '\t'))
		buf[--n] = '\0';

	return strdup(buf);
}

static unsigned long long read_sysfs_u64(const char *dir, const char *file)
{
	unsigned long long v = 0;
	char *s = read_sysfs(dir, file);

	if (s) {
		v = strtoull(s, NULL, 0);
		free(s);
	}

	return v;
}

/*
 * The GPU's runtime-PM control file and its saved value. While the GPU is
 * runtime-suspended its devfreq is suspended too and cur_freq freezes, so the
 * ramp-down test pins the device resumed for the observation and restores the
 * original setting afterwards.
 */
static char rpm_control_path[256];
static char rpm_control_saved[16];

static void restore_runtime_pm(int sig)
{
	int wfd, ret;

	if (!rpm_control_path[0] || !rpm_control_saved[0])
		return;

	wfd = open(rpm_control_path, O_WRONLY);
	if (wfd < 0)
		return;
	ret = write(wfd, rpm_control_saved, strlen(rpm_control_saved));
	(void)ret;
	close(wfd);
}

/*
 * Pin the GPU runtime-resumed (power/control = "on") so devfreq keeps polling
 * and the governor can ramp the frequency down while the GPU is idle. Restored
 * via an exit handler as a backstop. Drivers without runtime PM are unaffected.
 * Returns false if the control file is unavailable.
 */
static bool hold_runtime_pm_resumed(int fd)
{
	char pdir[224];
	struct stat st;
	char *cur;
	int wfd;

	if (fstat(fd, &st) < 0)
		return false;

	snprintf(pdir, sizeof(pdir), "/sys/dev/char/%u:%u/device/power",
		 major(st.st_rdev), minor(st.st_rdev));

	cur = read_sysfs(pdir, "control");
	if (!cur)
		return false;
	snprintf(rpm_control_saved, sizeof(rpm_control_saved), "%s", cur);
	free(cur);

	snprintf(rpm_control_path, sizeof(rpm_control_path), "%s/control", pdir);
	wfd = open(rpm_control_path, O_WRONLY);
	if (wfd < 0)
		return false;
	if (write(wfd, "on\n", 3) != 3) {
		close(wfd);
		return false;
	}
	close(wfd);

	igt_install_exit_handler(restore_runtime_pm);
	return true;
}

/* Count the operating points exposed via available_frequencies. */
static unsigned int count_opps(const char *dir)
{
	char *s = read_sysfs(dir, "available_frequencies");
	unsigned int count = 0;
	char *tok, *save;

	if (!s)
		return 0;

	for (tok = strtok_r(s, " ", &save); tok; tok = strtok_r(NULL, " ", &save))
		count++;

	free(s);

	return count;
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
 * A long counted-loop workload: its handles and the mapped scratch BO. Submit
 * it with submit_long_workload(), wait on .syncobj, then tear it down.
 */
struct workload {
	uint32_t vm_id;
	uint32_t group_handle;
	uint32_t syncobj;
	struct panthor_bo bo;
};

static void submit_long_workload(int fd, struct workload *w)
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

	ninstrs = emit_counted_loop(instrs, INITIAL_VA + COUNTER_OFFSET,
				    DEVFREQ_LOOP_N);
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

static void teardown_workload(int fd, struct workload *w)
{
	igt_panthor_group_destroy(fd, w->group_handle, 0);
	syncobj_destroy(fd, w->syncobj);
	igt_panthor_free_bo(fd, &w->bo);
	igt_panthor_vm_destroy(fd, w->vm_id, 0);
}

int igt_main()
{
	int fd = -1;
	char *devfreq = NULL;

	igt_fixture() {
		struct drm_panthor_gpu_info gpu_info = {};

		igt_panthor_skip_on_big_endian();
		fd = drm_open_driver(DRIVER_PANTHOR);

		igt_panthor_query(fd, DRM_PANTHOR_DEV_QUERY_GPU_INFO,
				  &gpu_info, sizeof(gpu_info), 0);
		g_shader_present = gpu_info.shader_present;
		g_tiler_present = gpu_info.tiler_present;

		/*
		 * devfreq is optional. If there is no devfreq instance for the
		 * GPU (PM_DEVFREQ off, or no OPP table), skip the whole suite.
		 */
		devfreq = find_devfreq_sysfs(fd);
		igt_require_f(devfreq != NULL,
			      "no devfreq sysfs directory; is CONFIG_PM_DEVFREQ enabled?\n");
	}

	igt_describe("Verify the devfreq sysfs surface is present and exposes "
		     "the simple_ondemand governor.");
	igt_subtest("sysfs") {
		char *cur, *gov, *avail;

		cur = read_sysfs(devfreq, "cur_freq");
		igt_assert_f(cur != NULL, "cur_freq missing\n");
		free(cur);

		gov = read_sysfs(devfreq, "governor");
		igt_assert_f(gov != NULL, "governor attribute missing\n");
		/*
		 * The active governor is configurable, so require rather than
		 * fail if it is not simple_ondemand.
		 */
		igt_require_f(strcmp(gov, "simple_ondemand") == 0,
			      "active governor is '%s', expected 'simple_ondemand'\n",
			      gov);
		free(gov);

		avail = read_sysfs(devfreq, "available_governors");
		igt_assert_f(avail != NULL, "available_governors missing\n");
		igt_assert_f(strstr(avail, "simple_ondemand") != NULL,
			     "simple_ondemand absent from available_governors: '%s'\n",
			     avail);
		free(avail);
	}

	igt_describe("Verify cur_freq ramps up above the idle baseline under a "
		     "sustained compute load.");
	igt_subtest("ramp_up") {
		unsigned long long baseline, peak;
		struct workload w = {};
		char *gov;

		gov = read_sysfs(devfreq, "governor");
		igt_require_f(gov && strcmp(gov, "simple_ondemand") == 0,
			      "ramp test needs the simple_ondemand governor\n");
		free(gov);
		igt_require_f(count_opps(devfreq) > 1,
			      "need more than one OPP for the governor to scale\n");

		/* Let the governor settle at its idle baseline. */
		sleep(1);
		baseline = read_sysfs_u64(devfreq, "cur_freq");
		igt_info("baseline cur_freq = %llu Hz\n", baseline);

		submit_long_workload(fd, &w);

		/* Poll cur_freq for up to ~2s; stop once it rises. */
		peak = baseline;
		for (int i = 0; i < 20; i++) {
			unsigned long long f;

			usleep(100000);
			f = read_sysfs_u64(devfreq, "cur_freq");
			if (f > peak)
				peak = f;
			if (peak > baseline)
				break;
		}
		igt_info("peak cur_freq under load = %llu Hz\n", peak);
		igt_assert_f(peak > baseline,
			     "cur_freq did not rise above baseline %llu (peak %llu)\n",
			     baseline, peak);

		/* Generous budget: clock may be starved on a non-devfreq pin. */
		igt_assert_f(wait_done(fd, w.syncobj, 60 * SEC_NS),
			     "workload timed out\n");
		teardown_workload(fd, &w);
	}

	igt_describe("Verify cur_freq ramps back down after the workload stops "
		     "and the GPU goes idle.");
	igt_subtest("ramp_down") {
		unsigned long long peak = 0, idle;
		struct workload w = {};
		char *gov;

		gov = read_sysfs(devfreq, "governor");
		igt_require_f(gov && strcmp(gov, "simple_ondemand") == 0,
			      "ramp test needs the simple_ondemand governor\n");
		free(gov);
		igt_require_f(count_opps(devfreq) > 1,
			      "need more than one OPP for the governor to scale\n");
		igt_require_f(hold_runtime_pm_resumed(fd),
			      "cannot pin runtime PM to observe ramp-down\n");

		submit_long_workload(fd, &w);

		/* Drive the freq up and record the peak. */
		for (int i = 0; i < 20; i++) {
			unsigned long long f;

			usleep(100000);
			f = read_sysfs_u64(devfreq, "cur_freq");
			if (f > peak)
				peak = f;
		}
		igt_assert_f(wait_done(fd, w.syncobj, 60 * SEC_NS),
			     "workload timed out\n");
		teardown_workload(fd, &w);
		igt_info("peak cur_freq under load = %llu Hz\n", peak);

		/*
		 * The governor re-evaluates only on its polling interval, so
		 * poll for the drop rather than sampling once after a fixed
		 * delay.
		 */
		idle = peak;
		for (int i = 0; i < 150; i++) {
			usleep(100000);
			idle = read_sysfs_u64(devfreq, "cur_freq");
			if (idle < peak)
				break;
		}
		restore_runtime_pm(0);
		igt_info("idle cur_freq after ramp-down wait = %llu Hz\n", idle);
		igt_assert_f(idle < peak,
			     "cur_freq did not drop after idle (peak %llu, idle %llu)\n",
			     peak, idle);
	}

	igt_describe("Verify trans_stat is present and records frequency "
		     "transitions after a load/idle cycle.");
	igt_subtest("trans_stat") {
		const char *marker = "Total transition : ";
		struct workload w = {};
		unsigned long long n;
		char *ts, *p;

		/*
		 * Drive at least one transition so the count is meaningful even
		 * when this subtest runs in isolation.
		 */
		if (count_opps(devfreq) > 1) {
			submit_long_workload(fd, &w);
			for (int i = 0; i < 20; i++)
				usleep(100000);
			igt_assert_f(wait_done(fd, w.syncobj, 60 * SEC_NS),
				     "workload timed out\n");
			teardown_workload(fd, &w);
			sleep(2);
		}

		ts = read_sysfs(devfreq, "trans_stat");
		igt_assert_f(ts != NULL, "trans_stat missing\n");

		p = strstr(ts, marker);
		igt_assert_f(p != NULL,
			     "trans_stat missing 'Total transition :' line\n");
		n = strtoull(p + strlen(marker), NULL, 0);
		igt_assert_f(n > 0, "trans_stat Total transitions still 0\n");
		free(ts);
	}

	igt_fixture() {
		free(devfreq);
		drm_close_driver(fd);
	}
}
