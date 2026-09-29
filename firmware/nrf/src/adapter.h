/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <stdint.h>
#include "link.h"
#define PCM_SAMPLES 480
#define PCM_BYTES (PCM_SAMPLES * 2 * sizeof(int16_t))
extern atomic_t host_flags, ear_flags, link_errors, pcm_frames, pcm_peak;
extern atomic_t pairing_token, test_mode, reset_epoch;
extern uint32_t boot_id;
extern struct k_mutex name_mutex;
extern char pair_name[64];
extern char pair_hardware_revision[16];
void link_init(void);
int pcm_init(void);
void earables_init(void);
void earables_pair(void);
void earables_reset(void);
void earables_suspend(void);
void earables_resume(void);
void earables_radio_status(void);
void earables_send(const int16_t *stereo);
void earables_capture(int16_t *stereo);
void adapter_led(unsigned index, bool on);
