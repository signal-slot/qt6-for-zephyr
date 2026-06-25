// Stage 2 strong definitions for Qt Multimedia's Zephyr I2S audio backend.
//
// Codec is configured once. I2S stream uses DRAIN on close (plays
// remaining buffers to completion) and re-queues+starts on open.

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

static bool codec_configured;
static bool stream_running;
static int audio_block_size = AUDIO_BLOCK_SIZE;

extern "C" {

int qzephyr_audio_open(int sample_rate, int channels, int bits_per_sample)
{
    if (stream_running)
        return 0;

    if (!i2s_dev) {
        i2s_dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(i2s_tx));
        if (!i2s_dev || !device_is_ready(i2s_dev)) {
            printk("qzephyr_audio: i2s-tx not found/ready\n");
            return -ENODEV;
        }
    }

    int ret;

    if (!codec_configured) {
        // Start I2S with silence BEFORE codec configure so the codec
        // DAC sees a stable BCLK/LRCK from power-on → no pop.
        struct i2s_config pre_cfg = {};
        pre_cfg.word_size = bits_per_sample;
        pre_cfg.channels = channels;
        pre_cfg.format = I2S_FMT_DATA_FORMAT_I2S | I2S_FMT_CLK_NF_NB;
        pre_cfg.options = I2S_OPT_BIT_CLK_CONTROLLER
                        | I2S_OPT_FRAME_CLK_CONTROLLER;
        pre_cfg.frame_clk_freq = sample_rate;
        pre_cfg.mem_slab = &audio_slab;
        pre_cfg.block_size = AUDIO_BLOCK_SIZE;
        pre_cfg.timeout = 2000;
        i2s_configure(i2s_dev, I2S_DIR_TX, &pre_cfg);

        static const char silence_pre[AUDIO_BLOCK_SIZE] = {};
        for (int i = 0; i < 2; i++)
            i2s_buf_write(i2s_dev, (void *)silence_pre, AUDIO_BLOCK_SIZE);
        i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
        printk("qzephyr_audio: I2S clock pre-running\n");

        k_msleep(50);

#if defined(CONFIG_AUDIO_CODEC) && DT_NODE_HAS_STATUS(DT_NODELABEL(audio_codec), okay)
        const struct device *codec_dev = DEVICE_DT_GET(DT_NODELABEL(audio_codec));
        if (device_is_ready(codec_dev)) {
            struct audio_codec_cfg audio_cfg = {};
            audio_cfg.dai_route = AUDIO_ROUTE_PLAYBACK;
            audio_cfg.dai_type = AUDIO_DAI_TYPE_I2S;
            audio_cfg.dai_cfg.i2s.word_size = bits_per_sample;
            audio_cfg.dai_cfg.i2s.channels = channels;
            audio_cfg.dai_cfg.i2s.format = I2S_FMT_DATA_FORMAT_I2S;
            audio_cfg.dai_cfg.i2s.options = I2S_OPT_FRAME_CLK_TARGET
                                          | I2S_OPT_BIT_CLK_TARGET;
            audio_cfg.dai_cfg.i2s.frame_clk_freq = sample_rate;
            audio_cfg.dai_cfg.i2s.mem_slab = &audio_slab;
            audio_cfg.dai_cfg.i2s.block_size = AUDIO_BLOCK_SIZE;
            ret = audio_codec_configure(codec_dev, &audio_cfg);
            if (ret < 0)
                printk("qzephyr_audio: codec configure failed: %d\n", ret);
            else
                printk("qzephyr_audio: codec configured (clock was pre-running)\n");
            k_msleep(200);
        }
#endif

        i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
        codec_configured = true;
    }

    // Configure I2S (valid from NOT_READY or READY states).
    struct i2s_config cfg = {};
    cfg.word_size = bits_per_sample;
    cfg.channels = channels;
    cfg.format = I2S_FMT_DATA_FORMAT_I2S | I2S_FMT_CLK_NF_NB;
    cfg.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER;
    cfg.frame_clk_freq = sample_rate;
    cfg.mem_slab = &audio_slab;
    cfg.block_size = AUDIO_BLOCK_SIZE;
    cfg.timeout = 2000;

    ret = i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
    if (ret < 0) {
        printk("qzephyr_audio: i2s_configure=%d\n", ret);
        return ret;
    }

    // Prime and start.
    {
        static const char silence[AUDIO_BLOCK_SIZE] = {};
        for (int i = 0; i < 2; i++) {
            ret = i2s_buf_write(i2s_dev, (void *)silence, AUDIO_BLOCK_SIZE);
            if (ret < 0) {
                printk("qzephyr_audio: buf_write=%d\n", ret);
                return ret;
            }
        }
    }

    ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
    if (ret < 0) {
        printk("qzephyr_audio: START=%d\n", ret);
        return ret;
    }

    stream_running = true;
    printk("qzephyr_audio: opened %d Hz %d ch %d bit\n",
           sample_rate, channels, bits_per_sample);
    return 0;
}

void qzephyr_audio_close(void)
{
    if (!stream_running)
        return;

    // DRAIN plays out all queued buffers then returns I2S to READY.
    i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
    stream_running = false;
}

int qzephyr_audio_write(const void *data, int size)
{
    if (!stream_running || !i2s_dev)
        return -ENODEV;

    const uint8_t *src = (const uint8_t *)data;
    int total_written = 0;

    while (size > 0) {
        int chunk = (size < audio_block_size) ? size : audio_block_size;

        int ret = i2s_buf_write(i2s_dev, (void *)src, chunk);
        if (ret == -ENOMEM)
            break;
        if (ret < 0) {
            if (total_written > 0)
                break;
            return ret;
        }

        src += chunk;
        size -= chunk;
        total_written += chunk;
    }

    return total_written;
}

int qzephyr_audio_bytes_free(void)
{
    if (!stream_running)
        return 0;
    uint32_t used = k_mem_slab_num_used_get(&audio_slab);
    uint32_t free_blocks = AUDIO_BLOCK_COUNT - used;
    return free_blocks * audio_block_size;
}

} // extern "C"
