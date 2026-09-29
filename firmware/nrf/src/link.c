/* SPDX-License-Identifier: Apache-2.0 */
#include "adapter.h"
#include <nrfx_spis.h>
#include <hal/nrf_gpio.h>
#include <zephyr/irq.h>
static const nrfx_spis_t spis = NRFX_SPIS_INSTANCE(3);
static struct oe_link_frame tx __aligned(4), rx __aligned(4);
static K_SEM_DEFINE(done, 0, 1);
atomic_t spi_length_errors, spi_crc_errors, spi_buffer_errors;
static void event(nrfx_spis_evt_t const *e, void *context) {
    if (e->evt_type == NRFX_SPIS_BUFFERS_SET_DONE)
        nrf_gpio_pin_set(NRF_GPIO_PIN_MAP(1, 11));
    if (e->evt_type == NRFX_SPIS_XFER_DONE) {
        nrf_gpio_pin_clear(NRF_GPIO_PIN_MAP(1, 11));
        if (e->rx_amount != sizeof(rx)) {
            atomic_inc(&spi_length_errors);
            atomic_inc(&link_errors);
        }
        k_sem_give(&done);
    }
}
static void run(void *a, void *b, void *c) {
    uint32_t sequence = 0;
    for (;;) {
        memset(&tx, 0, sizeof(tx));
        tx.sequence = ++sequence;
        tx.pairing_token = atomic_get(&pairing_token);
        tx.pcm_frames = atomic_get(&pcm_frames);
        tx.errors = atomic_get(&link_errors);
        tx.peak = atomic_get(&pcm_peak);
        tx.boot_id = boot_id;
        k_mutex_lock(&name_mutex, K_FOREVER);
        tx.flags = atomic_get(&ear_flags) | (atomic_get(&test_mode) ? OE_LINK_TEST : 0);
        tx.reset_epoch = atomic_get(&reset_epoch);
        memcpy(tx.name, pair_name, sizeof(tx.name));
        memcpy(tx.hardware_revision, pair_hardware_revision, sizeof(tx.hardware_revision));
        k_mutex_unlock(&name_mutex);
        oe_link_seal(&tx);
        int err =
            nrfx_spis_buffers_set(&spis, (uint8_t *)&tx, sizeof(tx), (uint8_t *)&rx, sizeof(rx));
        if (err != NRFX_SUCCESS) {
            atomic_inc(&spi_buffer_errors);
            atomic_inc(&link_errors);
            k_sleep(K_MSEC(20));
            continue;
        }
        // Keep DMA buffers owned by SPIS until completion, but expire stale
        // host state when the ESP resets or stops supplying valid frames.
        while (k_sem_take(&done, K_MSEC(100)))
            atomic_set(&host_flags, 0);
        if (oe_link_valid(&rx))
            atomic_set(&host_flags, rx.flags);
        else {
            atomic_inc(&spi_crc_errors);
            atomic_set(&host_flags, 0);
            atomic_inc(&link_errors);
        }
    }
}
static K_THREAD_STACK_DEFINE(stack, 1536);
static struct k_thread thread;
void link_init(void) {
    nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(1, 11));
    nrf_gpio_pin_clear(NRF_GPIO_PIN_MAP(1, 11));
    nrfx_spis_config_t cfg =
        NRFX_SPIS_DEFAULT_CONFIG(NRF_GPIO_PIN_MAP(1, 15), NRF_GPIO_PIN_MAP(1, 13),
                                 NRF_GPIO_PIN_MAP(1, 14), NRF_GPIO_PIN_MAP(1, 12));
    cfg.mode = NRF_SPIS_MODE_0;
    cfg.bit_order = NRF_SPIS_BIT_ORDER_MSB_FIRST;
    IRQ_CONNECT(NRFX_IRQ_NUMBER_GET(NRF_SPIS3), 4, nrfx_spis_3_irq_handler, 0, 0);
    int err = nrfx_spis_init(&spis, &cfg, event, NULL);
    printk("LINK init=%d size=%u\n", err, (unsigned)sizeof(tx));
    if (err == NRFX_SUCCESS)
        k_thread_create(&thread, stack, K_THREAD_STACK_SIZEOF(stack), run, NULL, NULL, NULL, 5, 0,
                        K_NO_WAIT);
}
