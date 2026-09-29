/* SPDX-License-Identifier: Apache-2.0 */
#include "adapter.h"
#include <zephyr/drivers/i2s.h>
#include <stdlib.h>
#include <hal/nrf_clock.h>
static const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(i2s0));
#define WIRE_BYTES (PCM_SAMPLES * 2 * sizeof(int32_t))
K_MEM_SLAB_DEFINE_STATIC(rx_slab, WIRE_BYTES, 8, 4);
K_MEM_SLAB_DEFINE_STATIC(tx_slab, WIRE_BYTES, 8, 4);
static int16_t input[PCM_SAMPLES * 2];
static int16_t output[PCM_SAMPLES * 2];
/* Debugger-readable 100 ms capture; 1=request, 2=capturing, 3=complete. */
atomic_t pcm_snapshot_state;
atomic_t pcm_recoveries, pcm_process_max_us;
atomic_t pcm_padding_errors;
/* Diagnostic fault injection is only armed explicitly on the adapter debugger. */
atomic_t pcm_inject_delay_ms;
int16_t mic_snapshot[PCM_SAMPLES * 2 * 10];
int16_t pcm_snapshot[PCM_SAMPLES * 2 * 10];
static int start(void) {
    for (unsigned i = 0; i < 3; i++) {
        void *block;
        int err = k_mem_slab_alloc(&tx_slab, &block, K_NO_WAIT);
        if (err)
            return err;
        memset(block, 0, WIRE_BYTES);
        err = i2s_write(dev, block, WIRE_BYTES);
        if (err) {
            k_mem_slab_free(&tx_slab, block);
            return err;
        }
    }
    return i2s_trigger(dev, I2S_DIR_BOTH, I2S_TRIGGER_START);
}
static void recover(void) {
    /* ERROR has already stopped and uninitialized nrfx I2S in its IRQ.
     * DROP calls nrfx_i2s_stop() again and asserts on that uninitialized
     * instance (NCS 3.0.1). PREPARE is the documented ERROR -> READY path.
     * Let the asynchronous STOPPED callback finish before reinitializing. */
    atomic_inc(&pcm_recoveries);
    k_sleep(K_MSEC(20));
    int err = i2s_trigger(dev, I2S_DIR_BOTH, I2S_TRIGGER_PREPARE);
    if (!err)
        err = start();
    printk("PCM recovery=%d\n", err);
    if (err)
        k_sleep(K_MSEC(100));
}
static void run(void *a, void *b, void *c) {
    uint32_t phase = 0;
    unsigned snapshot_block = 0;
    for (;;) {
        int delay = atomic_set(&pcm_inject_delay_ms, 0);
        if (delay > 0 && delay <= 200)
            k_sleep(K_MSEC(delay));
        void *block;
        size_t size;
        int err = i2s_read(dev, &block, &size);
        if (err) {
            recover();
            continue;
        }
        uint32_t begin = k_cycle_get_32();
        /* Use 32-bit slots with zero padding after the 16-bit PCM sample.
         * The ESP32 slave otherwise corrupts the next slot at a word boundary. */
        int32_t *wire = block;
        for (unsigned i = 0; i < ARRAY_SIZE(input); i++) {
            if (i < size / sizeof(*wire) && (wire[i] & 0xffff))
                atomic_inc(&pcm_padding_errors);
            input[i] = i < size / sizeof(*wire) ? wire[i] >> 16 : 0;
        }
        k_mem_slab_free(&rx_slab, block);
        if (!(atomic_get(&host_flags) & (OE_HOST_PLAYING | OE_HOST_MIC_ACTIVE)))
            memset(input, 0, sizeof(input));
        if (atomic_get(&pcm_snapshot_state) == 1) {
            snapshot_block = 0;
            atomic_set(&pcm_snapshot_state, 2);
        }
        if (atomic_get(&pcm_snapshot_state) == 2) {
            memcpy(pcm_snapshot + snapshot_block * PCM_SAMPLES * 2, input, sizeof(input));
            /* Mark complete only after recording the corresponding return block. */
        }
        int peak = 0;
        for (int i = 0; i < PCM_SAMPLES * 2; i++)
            peak = MAX(peak, abs(input[i]));
        atomic_set(&pcm_peak, peak);
        atomic_inc(&pcm_frames);
        earables_send(input);
        if (k_mem_slab_alloc(&tx_slab, &block, K_MSEC(20)))
            continue;
        earables_capture(output);
        if (atomic_get(&pcm_snapshot_state) == 2) {
            memcpy(mic_snapshot + snapshot_block * PCM_SAMPLES * 2, output, sizeof(output));
            if (++snapshot_block == 10)
                atomic_set(&pcm_snapshot_state, 3);
        }
        if (atomic_get(&test_mode)) {
            int16_t *out = output;
            /* Digital fixture only: low-level 1 kHz square wave on the return wire. */
            for (int i = 0; i < PCM_SAMPLES; i++, phase++)
                out[2 * i] = out[2 * i + 1] = (phase % 48 < 24) ? 1000 : -1000;
        }
        wire = block;
        for (unsigned i = 0; i < ARRAY_SIZE(output); i++)
            wire[i] = (int32_t)output[i] * 65536;
        err = i2s_write(dev, block, WIRE_BYTES);
        if (err) {
            k_mem_slab_free(&tx_slab, block);
            recover();
        }
        uint32_t elapsed = k_cyc_to_us_floor32(k_cycle_get_32() - begin);
        if (elapsed > atomic_get(&pcm_process_max_us))
            atomic_set(&pcm_process_max_us, elapsed);
    }
}
static K_THREAD_STACK_DEFINE(stack, 8192);
static struct k_thread thread;
int pcm_init(void) {
    if (!device_is_ready(dev))
        return -ENODEV;
    /* Selecting ACLK did not start the audio PLL on the tested DK. Its
     * unstarted clock ran about 0.5% fast and ignored frequency adjustment.
     * Start it explicitly and verify its state before any I2S transfer. */
    nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_HFCLKAUDIOSTART);
    int64_t deadline = k_uptime_get() + 100;
    while (!(NRF_CLOCK->HFCLKAUDIOSTAT & CLOCK_HFCLKAUDIOSTAT_STATE_Msk)) {
        if (k_uptime_get() >= deadline)
            return -ETIMEDOUT;
        k_sleep(K_MSEC(1));
    }
    struct i2s_config cfg = {.word_size = 32,
                             .channels = 2,
                             .format = I2S_FMT_DATA_FORMAT_I2S,
                             .options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
                             .frame_clk_freq = 48000,
                             .block_size = WIRE_BYTES,
                             .timeout = 2000,
                             .mem_slab = &rx_slab};
    int err = i2s_configure(dev, I2S_DIR_RX, &cfg);
    if (err)
        return err;
    cfg.mem_slab = &tx_slab;
    err = i2s_configure(dev, I2S_DIR_TX, &cfg);
    if (err)
        return err;
    err = start();
    if (err)
        return err;
    k_thread_create(&thread, stack, K_THREAD_STACK_SIZEOF(stack), run, NULL, NULL, NULL, 3, 0,
                    K_NO_WAIT);
    return 0;
}
