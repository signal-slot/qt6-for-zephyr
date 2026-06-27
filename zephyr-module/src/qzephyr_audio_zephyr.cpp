// Stage 2 strong definitions for Qt Multimedia's Zephyr I2S audio backend.
//
// SAI1 TX runs continuously after first codec init to keep BCLK/LRCK alive
// for RX sync mode (nxp,rx-sync-mode). A dedicated silence-feeder thread
// writes zeroes to TX whenever no playback data is queued.

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/sys/printk.h>

#ifdef CONFIG_AUDIO_CODEC
#include <zephyr/audio/codec.h>
#endif

#include <string.h>

#define AUDIO_BLOCK_SIZE  4096
#define AUDIO_BLOCK_COUNT 8

static const struct device *i2s_dev;

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(audio_slab, __nocache,
                                 AUDIO_BLOCK_SIZE, AUDIO_BLOCK_COUNT, 4);

static volatile bool tx_alive;
static volatile bool playback_active;
static volatile bool stream_running;

// ---- TX silence feeder ----
// Keeps TX I2S running with silence when no playback is active.
// Required: RX sync mode borrows TX's BCLK/LRCK.

static void tx_silence_entry(void *, void *, void *)
{
    static const char silence[AUDIO_BLOCK_SIZE] = {};

    while (1) {
        if (!tx_alive || playback_active) {
            k_msleep(10);
            continue;
        }

        int ret = i2s_buf_write(i2s_dev, (void *)silence, AUDIO_BLOCK_SIZE);
        if (ret == -EAGAIN || ret == -ENOMEM) {
            k_msleep(5);
        } else if (ret < 0) {
            printk("qzephyr_audio: silence write=%d\n", ret);
            k_msleep(50);
        }
    }
}

#define TX_SILENCE_STACK_SIZE 1024
K_THREAD_STACK_DEFINE(tx_silence_stack, TX_SILENCE_STACK_SIZE);
static struct k_thread tx_silence_thread_data;
static bool tx_silence_started;

// ---- Shared init helpers ----

static int tx_ensure_alive(int sample_rate, int channels, int bits_per_sample)
{
    if (tx_alive)
        return 0;

    if (!i2s_dev) {
        i2s_dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(i2s_tx));
        if (!i2s_dev || !device_is_ready(i2s_dev)) {
            printk("qzephyr_audio: i2s-tx not ready\n");
            return -ENODEV;
        }
    }

    struct i2s_config cfg = {};
    cfg.word_size = bits_per_sample;
    cfg.channels = channels;
    cfg.format = I2S_FMT_DATA_FORMAT_I2S | I2S_FMT_CLK_NF_NB;
    cfg.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER;
    cfg.frame_clk_freq = sample_rate;
    cfg.mem_slab = &audio_slab;
    cfg.block_size = AUDIO_BLOCK_SIZE;
    cfg.timeout = 2000;

    int ret = i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
    if (ret < 0) {
        printk("qzephyr_audio: i2s_configure TX=%d\n", ret);
        return ret;
    }

    static const char silence[AUDIO_BLOCK_SIZE] = {};
    for (int i = 0; i < 2; i++) {
        ret = i2s_buf_write(i2s_dev, (void *)silence, AUDIO_BLOCK_SIZE);
        if (ret < 0) {
            printk("qzephyr_audio: prime write=%d\n", ret);
            return ret;
        }
    }

    ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
    if (ret < 0) {
        printk("qzephyr_audio: START TX=%d\n", ret);
        return ret;
    }

    tx_alive = true;

    if (!tx_silence_started) {
        k_thread_create(&tx_silence_thread_data, tx_silence_stack,
                        K_THREAD_STACK_SIZEOF(tx_silence_stack),
                        tx_silence_entry, NULL, NULL, NULL,
                        K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
        tx_silence_started = true;
    }

    printk("qzephyr_audio: TX alive %d Hz %d ch %d bit\n",
           sample_rate, channels, bits_per_sample);
    return 0;
}

// Codec PLAYBACK and CAPTURE routes are configured once each.
// PLAYBACK must come first (enables DAC + clock path).
static bool playback_route_done;
static bool capture_route_done;

static int ensure_codec_ready(int sample_rate, int channels, int bits_per_sample)
{
    int ret = tx_ensure_alive(sample_rate, channels, bits_per_sample);
    if (ret < 0)
        return ret;

    if (playback_route_done)
        return 0;

#if defined(CONFIG_AUDIO_CODEC) && DT_NODE_HAS_STATUS(DT_NODELABEL(audio_codec), okay)
    const struct device *codec = DEVICE_DT_GET(DT_NODELABEL(audio_codec));
    if (!device_is_ready(codec))
        return 0;

    k_msleep(50);

    struct audio_codec_cfg cfg = {};
    cfg.dai_route = AUDIO_ROUTE_PLAYBACK;
    cfg.dai_type = AUDIO_DAI_TYPE_I2S;
    cfg.dai_cfg.i2s.word_size = bits_per_sample;
    cfg.dai_cfg.i2s.channels = channels;
    cfg.dai_cfg.i2s.format = I2S_FMT_DATA_FORMAT_I2S;
    cfg.dai_cfg.i2s.options = I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET;
    cfg.dai_cfg.i2s.frame_clk_freq = sample_rate;
    cfg.dai_cfg.i2s.mem_slab = &audio_slab;
    cfg.dai_cfg.i2s.block_size = AUDIO_BLOCK_SIZE;

    ret = audio_codec_configure(codec, &cfg);
    printk("qzephyr_audio: codec PLAYBACK=%d\n", ret);
    k_msleep(200);
#endif

    playback_route_done = true;
    return 0;
}

static int ensure_capture_route(int sample_rate, int channels, int bits_per_sample)
{
    if (capture_route_done)
        return 0;

    int ret = ensure_codec_ready(sample_rate, channels, bits_per_sample);
    if (ret < 0)
        return ret;

#if defined(CONFIG_AUDIO_CODEC) && DT_NODE_HAS_STATUS(DT_NODELABEL(audio_codec), okay)
    const struct device *codec = DEVICE_DT_GET(DT_NODELABEL(audio_codec));
    if (!device_is_ready(codec))
        return 0;

    struct audio_codec_cfg cfg = {};
    cfg.dai_route = AUDIO_ROUTE_CAPTURE;
    cfg.dai_type = AUDIO_DAI_TYPE_I2S;
    cfg.dai_cfg.i2s.word_size = bits_per_sample;
    cfg.dai_cfg.i2s.channels = channels;
    cfg.dai_cfg.i2s.format = I2S_FMT_DATA_FORMAT_I2S;
    cfg.dai_cfg.i2s.options = I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET;
    cfg.dai_cfg.i2s.frame_clk_freq = sample_rate;

    ret = audio_codec_configure(codec, &cfg);
    printk("qzephyr_audio: codec CAPTURE=%d\n", ret);
    if (ret == 0)
        capture_route_done = true;
    k_msleep(100);
#else
    capture_route_done = true;
#endif

    return 0;
}

// ---- Playback (TX) ----

extern "C" {

int qzephyr_audio_open(int sample_rate, int channels, int bits_per_sample)
{
    if (stream_running)
        return 0;

    int ret = ensure_codec_ready(sample_rate, channels, bits_per_sample);
    if (ret < 0)
        return ret;

    playback_active = true;
    stream_running = true;
    printk("qzephyr_audio: opened %d Hz %d ch %d bit\n",
           sample_rate, channels, bits_per_sample);
    return 0;
}

void qzephyr_audio_close(void)
{
    if (!stream_running)
        return;

    playback_active = false;
    stream_running = false;
    printk("qzephyr_audio: playback closed\n");
}

int qzephyr_audio_write(const void *data, int size)
{
    if (!stream_running || !i2s_dev)
        return -ENODEV;

    const uint8_t *src = (const uint8_t *)data;
    int written = 0;

    while (size > 0) {
        int chunk = (size < AUDIO_BLOCK_SIZE) ? size : AUDIO_BLOCK_SIZE;
        int ret = i2s_buf_write(i2s_dev, (void *)src, chunk);
        if (ret == -ENOMEM)
            break;
        if (ret < 0)
            return (written > 0) ? written : ret;

        src += chunk;
        size -= chunk;
        written += chunk;
    }
    return written;
}

int qzephyr_audio_bytes_free(void)
{
    if (!stream_running)
        return 0;
    uint32_t used = k_mem_slab_num_used_get(&audio_slab);
    return (int)(AUDIO_BLOCK_COUNT - used) * AUDIO_BLOCK_SIZE;
}

// ---- Recording (RX) ----

#define RX_BLOCK_SIZE  4096
#define RX_BLOCK_COUNT 8
#define RX_RING_SIZE   (RX_BLOCK_SIZE * 8)

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(rx_slab, __nocache,
                                 RX_BLOCK_SIZE, RX_BLOCK_COUNT, 4);

static volatile bool rx_running;

static uint8_t rx_ring[RX_RING_SIZE];
static volatile uint32_t rx_head;
static volatile uint32_t rx_tail;

static uint32_t ring_avail(void)
{
    uint32_t h = rx_head, t = rx_tail;
    return (h >= t) ? (h - t) : (RX_RING_SIZE - t + h);
}

static uint32_t ring_free(void)
{
    return RX_RING_SIZE - 1 - ring_avail();
}

static void ring_write(const uint8_t *src, uint32_t len)
{
    uint32_t h = rx_head;
    uint32_t first = RX_RING_SIZE - h;
    if (first > len)
        first = len;
    memcpy(rx_ring + h, src, first);
    if (len > first)
        memcpy(rx_ring, src + first, len - first);
    rx_head = (h + len) % RX_RING_SIZE;
}

static uint32_t ring_read(uint8_t *dst, uint32_t len)
{
    uint32_t avail = ring_avail();
    if (len > avail)
        len = avail;
    if (len == 0)
        return 0;
    uint32_t t = rx_tail;
    uint32_t first = RX_RING_SIZE - t;
    if (first > len)
        first = len;
    memcpy(dst, rx_ring + t, first);
    if (len > first)
        memcpy(dst + first, rx_ring, len - first);
    rx_tail = (t + len) % RX_RING_SIZE;
    return len;
}

int qzephyr_audio_input_open(int sample_rate, int channels, int bits_per_sample)
{
    printk("qzephyr_audio_in: open %d Hz %d ch %d bit\n",
           sample_rate, channels, bits_per_sample);

    int ret = ensure_capture_route(sample_rate, channels, bits_per_sample);
    if (ret < 0)
        return ret;

    struct i2s_config cfg = {};
    cfg.word_size = bits_per_sample;
    cfg.channels = channels;
    cfg.format = I2S_FMT_DATA_FORMAT_I2S | I2S_FMT_CLK_NF_NB;
    cfg.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER;
    cfg.frame_clk_freq = sample_rate;
    cfg.mem_slab = &rx_slab;
    cfg.block_size = RX_BLOCK_SIZE;
    cfg.timeout = 0;

    ret = i2s_configure(i2s_dev, I2S_DIR_RX, &cfg);
    if (ret < 0) {
        printk("qzephyr_audio_in: configure RX=%d\n", ret);
        return ret;
    }

    ret = i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_START);
    if (ret < 0) {
        printk("qzephyr_audio_in: START RX=%d\n", ret);
        return ret;
    }

    rx_head = rx_tail = 0;
    rx_running = true;
    printk("qzephyr_audio_in: RX started\n");
    return 0;
}

void qzephyr_audio_input_close(void)
{
    if (!rx_running)
        return;
    rx_running = false;
    i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
    printk("qzephyr_audio_in: closed\n");
}

int qzephyr_audio_input_read(void *data, int size)
{
    if (!rx_running || !i2s_dev) {
        memset(data, 0, size);
        return size;
    }

    while (1) {
        void *block = NULL;
        size_t block_sz = 0;
        int ret = i2s_read(i2s_dev, &block, &block_sz);
        if (ret != 0 || !block)
            break;
        uint32_t n = block_sz;
        if (n > ring_free())
            n = ring_free();
        if (n > 0)
            ring_write((const uint8_t *)block, n);
        k_mem_slab_free(&rx_slab, block);
    }

    return (int)ring_read((uint8_t *)data, (uint32_t)size);
}

} // extern "C"
