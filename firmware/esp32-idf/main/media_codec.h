/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <esp_a2dp_api.h>

using MediaPCMCallback = void (*)(const uint8_t *, uint32_t);
using MediaFormatCallback = void (*)(unsigned, unsigned);
void mediaCodecInit(MediaPCMCallback callback, MediaFormatCallback formatCallback);
void mediaCodecRegisterEndpoints(bool sbcOnly);
void mediaCodecConfigure(const esp_a2d_mcc_t &mcc);
void mediaCodecDisconnect();
void mediaCodecStreamChanged(bool active);
void mediaCodecReceive(esp_a2d_conn_hdl_t handle, esp_a2d_audio_buff_t *buffer);
void mediaCodecPrintStats();
unsigned mediaCodecRate(const esp_a2d_mcc_t &mcc);
unsigned mediaCodecChannels(const esp_a2d_mcc_t &mcc);
