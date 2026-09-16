/* Reset the SoC on a fatal error instead of halting.
 *
 * A halted core on the lab board cannot be recovered remotely: nothing can
 * power-cycle it, and the inherited RTI watchdog is still being fed by the
 * feeder thread on another core until its bounded lifetime ends (up to
 * CONFIG_SOC_AM62PX_A53_FEED_INHERITED_WDT_SECONDS).  Rebooting right after
 * Zephyr has printed the fault dump brings Linux back in about 40 s, so a
 * crash costs one reboot, not the whole watchdog window.  Same policy as the
 * GPU driver repository's samples (PVR_SAMPLE_REBOOT). */

#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);
	printk("qt-zephyr-port: fatal error %u: resetting the SoC\n", reason);
	k_busy_wait(200 * 1000);   /* let the console drain */
	sys_reboot(SYS_REBOOT_COLD);
}
