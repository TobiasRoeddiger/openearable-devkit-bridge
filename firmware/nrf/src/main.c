/* SPDX-License-Identifier: Apache-2.0 */
#include "adapter.h"
#include <zephyr/drivers/gpio.h>
#include <SEGGER_RTT.h>
#include <zephyr/random/random.h>
#include <hal/nrf_clock.h>
#include <nrfx_clock.h>
atomic_t host_flags, ear_flags, link_errors, pcm_frames, pcm_peak, pairing_token, test_mode,
    reset_epoch;
uint32_t boot_id;
K_MUTEX_DEFINE(name_mutex);
char pair_name[64] = "OpenEarable Adapter";
char pair_hardware_revision[16];
static const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
void adapter_led(unsigned index, bool on) { gpio_pin_set(gpio0, 28 + index, !on); }
int main(void) {
    /* Four LC3 operations per 10 ms frame need the full application CPU clock. */
    nrfx_err_t clock_error = nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK, NRF_CLOCK_HFCLK_DIV_1);
    if (clock_error != NRFX_SUCCESS) {
        printk("CPU clock error=%u\n", clock_error);
        return -EIO;
    }
    for (unsigned i = 0; i < 4; i++)
        gpio_pin_configure(gpio0, 28 + i, GPIO_OUTPUT_HIGH);
    gpio_pin_configure(gpio0, 23, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_configure(gpio0, 24, GPIO_INPUT | GPIO_PULL_UP);
    printk("OpenEarable adapter boot\n");
    boot_id = sys_rand32_get();
    earables_init();
    /* Load the saved reset epoch before publishing any SPI status. Otherwise
     * the ESP sees a transient zero at boot and clears its host bonds. */
    link_init();
    printk("PCM init=%d\n", pcm_init());
    int64_t combo = 0;
    bool combo_fired = false;
    int64_t pressed[2] = {0};
    bool fired[2] = {0};
    char line[80];
    unsigned used = 0;
    int64_t report = 0;
    for (;;) {
        int64_t now = k_uptime_get();
        bool both = gpio_pin_get(gpio0, 23) == 0 && gpio_pin_get(gpio0, 24) == 0;
        if (both) {
            if (!combo)
                combo = now;
            if (!combo_fired && now - combo >= 10000) {
                combo_fired = true;
                earables_reset();
            }
        } else {
            combo = 0;
            combo_fired = false;
        }
        for (unsigned i = 0; i < 2; i++) {
            if (both) {
                pressed[i] = 0;
                fired[i] = true;
                continue;
            }
            if (gpio_pin_get(gpio0, 23 + i) == 0) {
                if (!pressed[i])
                    pressed[i] = now;
                if (!fired[i] && now - pressed[i] >= 5000) {
                    fired[i] = true;
                    if (i == 0)
                        earables_pair();
                    else
                        atomic_inc(&pairing_token);
                }
            } else {
                /* A short press retries the saved set, for example when its
                 * second member has just been switched back on. */
                if (i == 0 && pressed[i] && !fired[i] && now - pressed[i] >= 50 &&
                    now - pressed[i] < 1000)
                    earables_resume();
                pressed[i] = 0;
                fired[i] = false;
            }
        }
        char c;
        while (SEGGER_RTT_Read(0, &c, 1)) {
            if (c == '\n' || c == '\r') {
                line[used] = 0;
                if (!strcmp(line, "PAIR"))
                    earables_pair();
                else if (!strcmp(line, "RESET"))
                    earables_reset();
                else if (!strcmp(line, "SUSPEND"))
                    earables_suspend();
                else if (!strcmp(line, "RECONNECT"))
                    earables_resume();
                else if (!strcmp(line, "RADIO"))
                    earables_radio_status();
                else if (!strcmp(line, "HOSTPAIR"))
                    atomic_inc(&pairing_token);
                else if (!strcmp(line, "TEST ON"))
                    atomic_set(&test_mode, 1);
                else if (!strcmp(line, "TEST OFF"))
                    atomic_set(&test_mode, 0);
                else if (!strcmp(line, "CLOCK"))
                    printk("CLOCK base=%p run=%lu stat=%lu freq=%lu\n", NRF_CLOCK,
                           (unsigned long)NRF_CLOCK->HFCLKAUDIORUN,
                           (unsigned long)NRF_CLOCK->HFCLKAUDIOSTAT,
                           (unsigned long)NRF_CLOCK->HFCLKAUDIO.FREQUENCY);
                else if (used)
                    printk("COMMAND unknown %s\n", line);
                used = 0;
            } else if (used < sizeof(line) - 1)
                line[used++] = c;
        }
        adapter_led(0, (now / 1000) % 2);
        adapter_led(1, (atomic_get(&ear_flags) & OE_EAR_PAIRING)
                           ? (now / 200) % 2
                           : (atomic_get(&ear_flags) & OE_EAR_CONNECTED));
        adapter_led(2, (atomic_get(&host_flags) & OE_HOST_PAIRABLE)
                           ? (now / 200) % 2
                           : (atomic_get(&host_flags) & OE_HOST_CONNECTED));
        adapter_led(3, atomic_get(&ear_flags) & OE_EAR_STREAMING);
        if (now - report >= 5000) {
            report = now;
            printk("STATUS frames=%ld peak=%ld errors=%ld host=%ld ear=%ld name=%s\n",
                   (long)atomic_get(&pcm_frames), (long)atomic_get(&pcm_peak),
                   (long)atomic_get(&link_errors), (long)atomic_get(&host_flags),
                   (long)atomic_get(&ear_flags), pair_name);
        }
        k_sleep(K_MSEC(20));
    }
}
