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

int qz_keepalive_link_anchor;

static void qzephyr_idle_keepalive(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	uint32_t n = 0;
	for (;;) {
		k_msleep(10000);
		printk("[hb] alive #%u uptime=%lld ms\n", ++n, k_uptime_get());
	}
}

K_THREAD_DEFINE(qz_keepalive_tid, 1024, qzephyr_idle_keepalive, NULL, NULL, NULL,
		-1 /* cooperative */, 0, 0);
