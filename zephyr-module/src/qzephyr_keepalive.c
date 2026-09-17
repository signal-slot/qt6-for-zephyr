/* Idle keep-alive thread (2026-06-15).
 *
 * Without this, the long-running Qt GUI firmware (CONFIG_PM off,
 * CONFIG_TICKLESS_KERNEL on) wedges into a SILENT hang after tens of minutes:
 * no Zephyr fatal print, animation frozen, only a dongle reset recovers it --
 * the signature of an idle / SysTick clock-gating stall (the same class as the
 * RT1176 CM4 WFI keep-alive handled in soc/nxp/imxrt/imxrt11xx/soc.c, which is
 * CM4-only, so the CM7 was unprotected).
 *
 * A dedicated cooperative-priority thread that wakes every 10 s keeps one kernel
 * timeout always pending plus a periodic wake-up, which prevents the stall; the
 * collidingmice soak then ran indefinitely (>13 h continuous, was 7-25 min to a
 * crash/hang before).  The 10 s uptime print doubles as a cheap UART liveness
 * signal and costs ~9 ms once per 10 s on the polling console (negligible vs the
 * ~23 ms per-frame PXP DMA).  Cooperative priority (-1) so it preempts the main
 * thread.
 *
 * qz_keepalive_link_anchor is referenced from the module CMakeLists via
 * -Wl,-u,qz_keepalive_link_anchor so the linker keeps this archive object: its
 * only other symbols are the K_THREAD_DEFINE registration, which nothing
 * references, so without the anchor ld would drop the whole TU (the same reason
 * the Qt app's main() needs --whole-archive).
 */
#include <zephyr/kernel.h>
#include <stdint.h>

/* YakoGL's call tracer and the PowerVR backend's kick/wait counters (weak:
 * absent in a raster-only firmware). */
const char *yakogl_debug_last_call(uint32_t *seq) __attribute__((weak));
void pvr_backend_counters(uint32_t *kicks, uint32_t *waits) __attribute__((weak));

int qz_keepalive_link_anchor;

static void qzephyr_idle_keepalive(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	uint32_t n = 0;
#ifdef CONFIG_SCHED_THREAD_USAGE_ALL
	uint64_t prev_busy = 0, prev_all = 0;
#endif
	uint32_t prev_kicks = 0, prev_waits = 0;
	for (;;) {
		k_msleep(10000);
		printk("[hb] alive #%u uptime=%lld ms\n", ++n, k_uptime_get());
#ifdef CONFIG_SCHED_THREAD_USAGE_ALL
		{
			/* CPU load over the last beat: a frame rate limited by the
			 * CPU shows ~100 %, one waiting on the GPU much less (a
			 * blocked fence wait counts as idle) */
			k_thread_runtime_stats_t st;

			if (k_thread_runtime_stats_all_get(&st) == 0 && st.execution_cycles > prev_all) {
				uint64_t busy = st.total_cycles - prev_busy;
				uint64_t all = st.execution_cycles - prev_all;

				printk("[hb] cpu busy %u%%\n", (unsigned)(busy * 100u / all));
				prev_busy = st.total_cycles;
				prev_all = st.execution_cycles;
			}
		}
#endif
		if (pvr_backend_counters) {
			uint32_t kicks = 0, waits = 0;

			pvr_backend_counters(&kicks, &waits);
			printk("[hb] gpu: %u kicks, %u blocking waits in the last beat\n",
			       (unsigned)(kicks - prev_kicks), (unsigned)(waits - prev_waits));
			prev_kicks = kicks;
			prev_waits = waits;
		}
		if (yakogl_debug_last_call) {
			/* where the GL client is: a stalled main thread shows the same
			 * call and count beat after beat */
			uint32_t seq = 0;
			const char *call = yakogl_debug_last_call(&seq);

			printk("[hb] gl: last call %s, %u calls\n", call ? call : "?", (unsigned)seq);
		}
	}
}

K_THREAD_DEFINE(qz_keepalive_tid, 1024, qzephyr_idle_keepalive, NULL, NULL, NULL,
		-1 /* cooperative */, 0, 0);
