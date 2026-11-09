/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Touch input of the official Raspberry Pi 7-inch display on a Raspberry Pi 4.
 *
 * The VideoCore firmware reads the panel's touch controller.  We give it the
 * address of a buffer with the SET_TOUCHBUF property tag, and it keeps the
 * buffer up to date.  The layout and the polling follow Linux
 * (drivers/input/touchscreen/raspberrypi-ts.c).  Single touch only: the
 * first active point of the buffer is followed until it lifts; after it lifts,
 * no other finger is taken until all fingers are up.
 *
 * Measured on the board (issue #26): the firmware accepts the tag from
 * Zephyr's rpi_fw_transfer() and writes the buffer; it writes num_points = 99
 * for a moment before updates, which the reader skips.
 */

#define DT_DRV_COMPAT signal_slot_rpi_fw_touch

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel/mm.h>
#include <zephyr/cache.h>
#include <zephyr/input/input.h>
#include <zephyr/input/input_touch.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>

#ifdef CONFIG_RASPBERRYPI_FIRMWARE
#include <rpi_fw.h>
#endif

/* The driver flushes its buffer at init and invalidates it before each poll.
 * Without both options these calls are stubs that return -ENOTSUP, which the
 * driver does not look at (include/zephyr/cache.h).
 */
BUILD_ASSERT(IS_ENABLED(CONFIG_CACHE_MANAGEMENT) && IS_ENABLED(CONFIG_DCACHE),
	     "rpi_fw_touch needs CONFIG_CACHE_MANAGEMENT and CONFIG_DCACHE: its cache calls "
	     "would otherwise do nothing");

/* With the firmware node, init() asks the firmware device for SET_TOUCHBUF,
 * so that device must be initialized first.  Both are POST_KERNEL; a larger
 * priority number runs later.  Same condition as Zephyr's
 * gpio_rpi_bcm283x.c.  CONFIG_CHECK_INIT_PRIORITIES (default y) catches the
 * same mistake after linking; this assertion fails earlier, with a clearer
 * message, and also when that check is off.
 * Without CONFIG_RASPBERRYPI_FIRMWARE (the GL build) the driver sends the
 * request itself and the priority symbol does not exist, so the assertion is
 * left out.
 */
#ifdef CONFIG_RASPBERRYPI_FIRMWARE
BUILD_ASSERT(CONFIG_INPUT_INIT_PRIORITY > CONFIG_RASPBERRYPI_FIRMWARE_INIT_PRIORITY,
	     "rpi_fw_touch must initialize after the Raspberry Pi firmware device "
	     "(CONFIG_INPUT_INIT_PRIORITY > CONFIG_RASPBERRYPI_FIRMWARE_INIT_PRIORITY)");
#endif

#define RPI_FW_TAG_SET_TOUCHBUF 0x0004801fU

/* The VideoCore sees the ARM's RAM through this alias (the one Linux's DMA
 * addresses carry on the Pi 4).
 */
#define VC_BUS_ALIAS 0xC0000000U
#define RAM_END      0x40000000U

/*
 * Without the `firmware` property and with CONFIG_RASPBERRYPI_FIRMWARE off
 * (the GL build, where YakoGL polls the property mailbox itself and Zephyr's
 * firmware node is off) the tag is sent with one request on the mailbox
 * registers, the same sequence as YakoGL's
 * rpi_mbox.c: Linux v6.17 drivers/mailbox/bcm2835-mailbox.c (registers) and
 * drivers/firmware/raspberrypi.c (property buffer, channel 8).  It is sent
 * once at init, before the application runs; afterwards the driver only reads
 * memory.
 */
#define MBOX_BASE      0xfe00b880U /* bus 0x7e00b880 */
#define MAIL0_RD       0x00
#define MAIL0_STA      0x18
#define MAIL1_WRT      0x20
#define MAIL1_STA      0x38
#define MBOX_FULL      BIT(31)
#define MBOX_EMPTY     BIT(30)
#define CHAN_PROPERTY  8
#define STATUS_SUCCESS 0x80000000U
#define TAG_RESPONSE   BIT(31)

#define MAX_POINTS         10
#define NPOINTS_INVALID    99
#define EVENT_TOUCH_DOWN   0
#define EVENT_TOUCH_CONTACT 2
#define NO_ID              0xff

struct rpi_fw_touch_point {
	uint8_t xh;
	uint8_t xl;
	uint8_t yh;
	uint8_t yl;
	uint8_t pressure;
	uint8_t area;
} __packed;

struct rpi_fw_touch_regs {
	uint8_t device_mode;
	uint8_t gesture_id;
	uint8_t num_points;
	struct rpi_fw_touch_point point[MAX_POINTS];
} __packed;

/* Linux gives the firmware a whole page. */
#define TOUCHBUF_SIZE 4096

struct rpi_fw_touch_config {
	struct input_touchscreen_common_config common;
	const struct device *fw;
	uint8_t *buf;
	uint32_t poll_interval_ms;
};

struct rpi_fw_touch_data {
	struct k_work_delayable work;
	const struct device *dev;
	uint8_t tracked_id;
	bool pressed;
	/* A touch ended while other fingers were still down: wait until all are up. */
	bool wait_all_up;
	uint16_t last_x;
	uint16_t last_y;
};

static void rpi_fw_touch_poll(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct rpi_fw_touch_data *data = CONTAINER_OF(dwork, struct rpi_fw_touch_data, work);
	const struct device *dev = data->dev;
	const struct rpi_fw_touch_config *cfg = dev->config;
	struct rpi_fw_touch_regs regs;

	/* The firmware writes the buffer behind the CPU's cache. */
	sys_cache_data_invd_range(cfg->buf, sizeof(regs));
	memcpy(&regs, cfg->buf, sizeof(regs));

	/* 99 is written while an update is in progress; more than 10 is not a
	 * valid count either.  Try again at the next poll.
	 */
	if (regs.num_points != NPOINTS_INVALID && regs.num_points <= MAX_POINTS) {
		const struct rpi_fw_touch_point *found = NULL;
		uint8_t found_id = NO_ID;

		for (int i = 0; i < regs.num_points; i++) {
			const struct rpi_fw_touch_point *p = &regs.point[i];
			uint8_t type = (p->xh >> 6) & 0x3;
			uint8_t id = (p->yh >> 4) & 0xf;

			if (type != EVENT_TOUCH_DOWN && type != EVENT_TOUCH_CONTACT) {
				continue;
			}
			if (data->pressed && id == data->tracked_id) {
				found = p;
				found_id = id;
				break;
			}
			if (found == NULL) {
				found = p;
				found_id = id;
			}
		}

		/* Follow the finger we already have; if it is gone, the touch
		 * has ended.  Another finger that is still down does not start a
		 * new touch until all fingers have lifted (a palm or a second
		 * finger would otherwise click where the remaining one is).
		 */
		if (found == NULL) {
			data->wait_all_up = false;
		}
		if (data->pressed && (found == NULL || found_id != data->tracked_id)) {
			input_report_key(dev, INPUT_BTN_TOUCH, 0, true, K_FOREVER);
			data->pressed = false;
			data->tracked_id = NO_ID;
			data->wait_all_up = (found != NULL);
		} else if (found != NULL && !data->wait_all_up) {
			uint16_t x = ((found->xh & 0xf) << 8) | found->xl;
			uint16_t y = ((found->yh & 0xf) << 8) | found->yl;

			if (!data->pressed || x != data->last_x || y != data->last_y) {
				input_touchscreen_report_pos(dev, x, y, K_FOREVER);
				input_report_key(dev, INPUT_BTN_TOUCH, 1, true, K_FOREVER);
				data->pressed = true;
				data->tracked_id = found_id;
				data->last_x = x;
				data->last_y = y;
			}
		}
	}

	k_work_reschedule(&data->work, K_MSEC(cfg->poll_interval_ms));
}

#ifndef CONFIG_RASPBERRYPI_FIRMWARE
/* A whole 64-byte cache line, so that the invalidate after the answer does
 * not write back a neighbour's dirty data over the firmware's reply.
 */
static uint32_t mbox_buf[16] __aligned(64);
#endif

/* Send SET_TOUCHBUF with the buffer's bus address; *arg is the answer. */
static int rpi_fw_touch_set_buf(const struct rpi_fw_touch_config *cfg, uint32_t *arg)
{
	if (cfg->fw != NULL) {
#ifdef CONFIG_RASPBERRYPI_FIRMWARE
		return rpi_fw_transfer(cfg->fw, RPI_FW_TAG_SET_TOUCHBUF, arg, sizeof(*arg));
#else
		return -ENOTSUP;
#endif
	}

#ifndef CONFIG_RASPBERRYPI_FIRMWARE
	mm_reg_t mbox;
	uint32_t msg;
	uint32_t *b = mbox_buf;

	device_map(&mbox, MBOX_BASE, 0x40, K_MEM_CACHE_NONE);
	if (mbox == 0) {
		return -ENOMEM;
	}

	b[0] = 7 * sizeof(uint32_t); /* total size */
	b[1] = 0;                    /* request */
	b[2] = RPI_FW_TAG_SET_TOUCHBUF;
	b[3] = sizeof(uint32_t);     /* value buffer size */
	b[4] = 0;                    /* request code */
	b[5] = *arg;
	b[6] = 0;                    /* end tag */
	sys_cache_data_flush_range(b, sizeof(mbox_buf));

	for (int t = 0; sys_read32(mbox + MAIL1_STA) & MBOX_FULL; t++) {
		if (t > 100000) {
			return -ETIMEDOUT;
		}
		k_busy_wait(1);
	}
	sys_write32((uint32_t)(k_mem_phys_addr(b) | VC_BUS_ALIAS) | CHAN_PROPERTY, mbox + MAIL1_WRT);

	for (int t = 0;; t++) {
		if (!(sys_read32(mbox + MAIL0_STA) & MBOX_EMPTY)) {
			msg = sys_read32(mbox + MAIL0_RD);
			if ((msg & 0xf) == CHAN_PROPERTY) {
				break;
			}
		}
		if (t > 1000000) {
			return -ETIMEDOUT;
		}
		k_busy_wait(1);
	}
	sys_cache_data_invd_range(b, sizeof(mbox_buf));
	if (b[1] != STATUS_SUCCESS || !(b[4] & TAG_RESPONSE)) {
		return -EIO;
	}
	*arg = b[5];
	return 0;
#else
	return -ENOTSUP;
#endif
}

static int rpi_fw_touch_init(const struct device *dev)
{
	const struct rpi_fw_touch_config *cfg = dev->config;
	struct rpi_fw_touch_data *data = dev->data;
	uintptr_t phys;
	uint32_t arg;
	int ret;

	if (cfg->fw != NULL && !device_is_ready(cfg->fw)) {
		printk("rpi_fw_touch: the firmware device is not ready\n");
		return -ENODEV;
	}

	phys = k_mem_phys_addr(cfg->buf);
	if (phys + TOUCHBUF_SIZE > RAM_END) {
		printk("rpi_fw_touch: the touch buffer at 0x%lx is not reachable by the VideoCore\n", phys);
		return -EINVAL;
	}

	memset(cfg->buf, 0, TOUCHBUF_SIZE);
	sys_cache_data_flush_range(cfg->buf, TOUCHBUF_SIZE);

	arg = (uint32_t)phys | VC_BUS_ALIAS;
	ret = rpi_fw_touch_set_buf(cfg, &arg);
	if (ret < 0) {
		printk("rpi_fw_touch: SET_TOUCHBUF failed (%d)\n", ret);
		return ret;
	}
	/* Linux treats a non-zero answer as a failure. */
	if (arg != 0) {
		printk("rpi_fw_touch: the firmware refused the touch buffer (answer 0x%08x)\n", arg);
		return -EIO;
	}

	data->dev = dev;
	data->tracked_id = NO_ID;
	data->pressed = false;
	data->wait_all_up = false;
	k_work_init_delayable(&data->work, rpi_fw_touch_poll);
	k_work_schedule(&data->work, K_MSEC(cfg->poll_interval_ms));

	printk("rpi_fw_touch: touch buffer at 0x%lx, polling every %u ms\n", phys, cfg->poll_interval_ms);
	return 0;
}

#define RPI_FW_TOUCH_INIT(n)                                                                       \
	static uint8_t rpi_fw_touch_buf_##n[TOUCHBUF_SIZE] __aligned(4096);                        \
	static struct rpi_fw_touch_data rpi_fw_touch_data_##n;                                     \
	static const struct rpi_fw_touch_config rpi_fw_touch_config_##n = {                        \
		.common = INPUT_TOUCH_DT_INST_COMMON_CONFIG_INIT(n),                               \
		.fw = COND_CODE_1(DT_INST_NODE_HAS_PROP(n, firmware),                              \
				  (DEVICE_DT_GET(DT_INST_PHANDLE(n, firmware))), (NULL)),          \
		.buf = rpi_fw_touch_buf_##n,                                                       \
		.poll_interval_ms = DT_INST_PROP(n, poll_interval_ms),                             \
	};                                                                                         \
	INPUT_TOUCH_STRUCT_CHECK(struct rpi_fw_touch_config);                                      \
	DEVICE_DT_INST_DEFINE(n, rpi_fw_touch_init, NULL, &rpi_fw_touch_data_##n,                  \
			      &rpi_fw_touch_config_##n, POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY,   \
			      NULL);

DT_INST_FOREACH_STATUS_OKAY(RPI_FW_TOUCH_INIT)
