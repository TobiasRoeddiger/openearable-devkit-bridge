/* Electrical bring-up only: no Bluetooth, audio, or earable connections. */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/adc.h>
#include <hal/nrf_saadc.h>
#include <SEGGER_RTT.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct device *const gpio = DEVICE_DT_GET(DT_NODELABEL(gpio1));
static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static const struct device *const adc = DEVICE_DT_GET(DT_NODELABEL(adc));
/* Bit order is shared with the ESP sketch and the host verifier. */
static const uint8_t output_pins[] = {4, 5, 7, 14, 11};
static const uint8_t input_pins[] = {15, 13, 12, 6};
static const struct gpio_dt_spec leds[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios),
};
static const struct gpio_dt_spec buttons[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(sw3), gpios),
};
static unsigned output_mask;
static bool driving;
static bool adc_ready;

/* The internal VDD measurement avoids needing another wire for voltage QA. */
static int vdd_mv(void)
{
    if (!adc_ready) return -1;
    int16_t raw = 0;
    struct adc_sequence seq = {
        .channels = BIT(0), .buffer = &raw, .buffer_size = sizeof(raw),
        .resolution = 12,
    };
    int err = adc_read(adc, &seq);
    if (err) return err;
    int32_t mv = raw;
    err = adc_raw_to_millivolts(600, ADC_GAIN_1_6, 12, &mv);
    return err ? err : mv;
}

static int safe_pins(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(output_pins); ++i) {
        int err = gpio_pin_configure(gpio, output_pins[i], GPIO_INPUT | GPIO_PULL_DOWN);
        if (err) return err;
    }
    for (size_t i = 0; i < ARRAY_SIZE(input_pins); ++i) {
        int err = gpio_pin_configure(gpio, input_pins[i], GPIO_INPUT | GPIO_PULL_DOWN);
        if (err) return err;
    }
    driving = false;
    output_mask = 0;
    return 0;
}

static int drive_pins(unsigned mask)
{
    int mv = vdd_mv();
    if (mv < 3100 || mv > 3450) return -ERANGE;
    for (size_t i = 0; i < ARRAY_SIZE(output_pins); ++i) {
        int err = gpio_pin_configure(gpio, output_pins[i],
            (mask & BIT(i)) ? GPIO_OUTPUT_HIGH : GPIO_OUTPUT_LOW);
        if (err) { safe_pins(); return err; }
    }
    driving = true;
    output_mask = mask & 31;
    return 0;
}

static void report(void)
{
    unsigned inputs = 0, pressed = 0;
    for (size_t i = 0; i < ARRAY_SIZE(input_pins); ++i)
        if (gpio_pin_get(gpio, input_pins[i]) > 0) inputs |= BIT(i);
    for (size_t i = 0; i < ARRAY_SIZE(buttons); ++i)
        if (gpio_pin_get_dt(&buttons[i]) > 0) pressed |= BIT(i);
    printk("@STATE NRF I=%02x O=%02x DRIVE=%u BUTTONS=%x VDD=%d\n",
           inputs, output_mask, driving, pressed, vdd_mv());
}

static void command(char *line)
{
    if (!strcmp(line, "READ")) { report(); return; }
    if (!strcmp(line, "SAFE")) { printk("@SAFE %d\n", safe_pins()); return; }
    unsigned mask;
    char extra;
    if (sscanf(line, "OUT %x %c", &mask, &extra) == 1 && mask <= 31) {
        printk("@OUT %d\n", drive_pins(mask)); return;
    }
    if (sscanf(line, "LEDS %x %c", &mask, &extra) == 1 && mask <= 15) {
        for (size_t i = 0; i < ARRAY_SIZE(leds); ++i)
            gpio_pin_set_dt(&leds[i], !!(mask & BIT(i)));
        printk("@LEDS OK\n"); return;
    }
    printk("@ERROR unknown command\n");
}

int main(void)
{
    if (!device_is_ready(gpio) || !device_is_ready(uart)) return -ENODEV;
    int err = safe_pins();
    if (err) return err;
    for (size_t i = 0; i < ARRAY_SIZE(leds); ++i) {
        err = gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
        if (err) return err;
        err = gpio_pin_configure_dt(&buttons[i], GPIO_INPUT);
        if (err) return err;
    }
    struct adc_channel_cfg cfg = {
        .gain = ADC_GAIN_1_6, .reference = ADC_REF_INTERNAL,
        .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 10),
        .channel_id = 0, .input_positive = NRF_SAADC_INPUT_VDD,
    };
    adc_ready = device_is_ready(adc) && adc_channel_setup(adc, &cfg) == 0;
    printk("@READY NRF WIRING_V1 VDD=%d\n", vdd_mv());
    char line[64];
    size_t used = 0;
    bool overflow = false;
    int64_t last = 0;
    while (true) {
        unsigned char c;
        if (SEGGER_RTT_Read(0, &c, 1) == 1) {
            if (c == '\n') {
                if (!overflow) { line[used] = 0; command(line); }
                else printk("@ERROR line too long\n");
                used = 0; overflow = false;
            } else if (c != '\r') {
                if (used < sizeof(line) - 1) line[used++] = c;
                else overflow = true;
            }
        } else k_msleep(1);
        if (k_uptime_get() - last >= 1000) {
            gpio_pin_toggle_dt(&leds[0]); last = k_uptime_get();
        }
    }
}
