/* SPDX-License-Identifier: Apache-2.0 */
#include "media_codec.h"
#include <esp_audio_dec.h>
#include <esp_audio_dec_reg.h>
#include <esp_aac_dec.h>
extern "C" {
#include <oi_codec_sbc.h>
}
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_idf_version.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <atomic>
#include <cstdio>

namespace {
struct Packet { esp_a2d_audio_buff_t *buffer; uint32_t generation; };
QueueHandle_t packets;
SemaphoreHandle_t decoderLock;
esp_audio_dec_handle_t decoder;
// Use Bluedroid's SBC implementation, already linked for HFP. The separate
// esp_audio_codec SBC wrapper shares OI symbols but expects a different consumed-
// byte convention. Mixing those implementations skipped eight of nine frames.
struct SbcDecoder {
    OI_CODEC_SBC_DECODER_CONTEXT context;
    OI_UINT32 data[CODEC_DATA_WORDS(2, SBC_CODEC_FAST_FILTER_BUFFERS)];
};
SbcDecoder *sbcDecoder;
MediaPCMCallback emitPCM;
MediaFormatCallback setFormat;
std::atomic<uint32_t> generation{0}, codecType{255}, rate{48000}, channels{2};
std::atomic<uint32_t> received{0}, bytes{0}, decodedFrames{0}, decodeCalls{0};
std::atomic<uint32_t> queueDrops{0}, stalePackets{0}, decodeErrors{0}, openErrors{0};
std::atomic<uint32_t> queuePeak{0}, decodeMaxUs{0}, rtpTimestampStep{0};
std::atomic<uint32_t> transportPackets{0}, sequenceMissing{0}, sequenceReordered{0};
std::atomic<uint32_t> stackQueuePeak{0}, stackQueueLimitHits{0};
std::atomic<bool> resetSequence{true};
uint16_t expectedSequence;
uint32_t previousTimestamp;
bool timestampValid;

void highWater(std::atomic<uint32_t> &value, uint32_t n) {
    uint32_t old = value.load();
    while (n > old && !value.compare_exchange_weak(old, n)) {}
}
void closeDecoder() {
    if (decoder) esp_audio_dec_close(decoder);
    decoder = nullptr;
    free(sbcDecoder);
    sbcDecoder = nullptr;
    timestampValid = false;
}
void decodeTask(void *) {
    // AAC-LC has at most 1024 samples/channel; advertise neither HE-AAC nor >2 channels.
    alignas(4) static uint8_t pcm[4096];
    Packet packet;
    for (;;) {
        xQueueReceive(packets, &packet, portMAX_DELAY);
        xSemaphoreTake(decoderLock, portMAX_DELAY);
        auto *buffer = packet.buffer;
        if ((!decoder && !sbcDecoder) || packet.generation != generation.load()) {
            stalePackets++;
        } else {
            // Some hosts timestamp AAC in milliseconds rather than PCM samples.
            // Report the observed step; use RTP sequence numbers for loss accounting.
            if (timestampValid) rtpTimestampStep = buffer->timestamp - previousTimestamp;
            previousTimestamp = buffer->timestamp;
            uint32_t frames = 0;
            esp_audio_dec_in_raw_t input = {};
            input.buffer = buffer->data;
            input.len = buffer->data_len;
            const int64_t start = esp_timer_get_time();
            bool valid = true;
            while (input.len > 0) {
                esp_audio_dec_out_frame_t output = {};
                output.buffer = pcm;
                output.len = sizeof(pcm);
                input.consumed = 0;
                esp_audio_err_t error;
                if (sbcDecoder) {
                    const OI_BYTE *cursor = input.buffer;
                    OI_UINT32 remaining = input.len, size = sizeof(pcm);
                    const auto status = OI_CODEC_SBC_DecodeFrame(&sbcDecoder->context, &cursor,
                                                               &remaining, (OI_INT16 *)pcm, &size);
                    input.consumed = input.len - remaining;
                    output.decoded_size = size;
                    error = OI_SUCCESS(status) ? ESP_AUDIO_ERR_OK : ESP_AUDIO_ERR_FAIL;
                } else {
                    error = esp_audio_dec_process(decoder, &input, &output);
                }
                if (error != ESP_AUDIO_ERR_OK || !input.consumed || input.consumed > input.len ||
                    output.decoded_size > sizeof(pcm)) {
                    decodeErrors++;
                    valid = false;
                    printf("CODEC decode_error=%d consumed=%lu remaining=%lu output=%lu\n", error,
                           (unsigned long)input.consumed, (unsigned long)input.len,
                           (unsigned long)output.decoded_size);
                    break;
                }
                decodeCalls++;
                frames += output.decoded_size / (2 * channels.load());
                if (output.decoded_size) emitPCM(output.buffer, output.decoded_size);
                input.buffer += input.consumed;
                input.len -= input.consumed;
            }
            highWater(decodeMaxUs, esp_timer_get_time() - start);
            decodedFrames += frames;
            timestampValid = valid;
        }
        xSemaphoreGive(decoderLock);
        esp_a2d_audio_buff_free(buffer);
    }
}
}

// Read-only instrumentation at the stack ingress, before its compressed queue.
// BT_HDR ABI verified against the pinned IDF 6.1 stack/include/stack/bt_types.h.
#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(6, 1, 0)
#error "Revalidate the BT_HDR diagnostic wrapper when changing ESP-IDF"
#endif
struct TransportHeader { uint16_t event, len, offset, sequence; };
static_assert(sizeof(TransportHeader) == 8);
extern "C" uint8_t __real_btc_a2dp_sink_enque_buf(TransportHeader *packet);
extern "C" uint8_t __wrap_btc_a2dp_sink_enque_buf(TransportHeader *packet) {
    const auto sequence = packet->sequence;
    if (resetSequence.exchange(false)) expectedSequence = sequence;
    const uint16_t distance = sequence - expectedSequence;
    if (distance && distance < 0x8000) sequenceMissing += distance;
    if (distance >= 0x8000) sequenceReordered++;
    if (distance < 0x8000) expectedSequence = sequence + 1;
    transportPackets++;
    const auto queued = __real_btc_a2dp_sink_enque_buf(packet);
    highWater(stackQueuePeak, queued);
    // Hitting the limit is pressure evidence, not an exact count of dropped packets.
    if (queued >= 25) stackQueueLimitHits++;
    return queued;
}

unsigned mediaCodecRate(const esp_a2d_mcc_t &mcc) {
    if (mcc.type == ESP_A2D_MCT_M24)
        return mcc.cie.m24_info.samp_freq2 & ESP_A2D_M24_CIE_SF2_48K ? 48000 : 44100;
    return mcc.cie.sbc_info.samp_freq & ESP_A2D_SBC_CIE_SF_48K ? 48000 : 44100;
}
unsigned mediaCodecChannels(const esp_a2d_mcc_t &mcc) {
    if (mcc.type == ESP_A2D_MCT_M24)
        return mcc.cie.m24_info.ch & ESP_A2D_M24_CIE_CH_1 ? 1 : 2;
    return mcc.cie.sbc_info.ch_mode & ESP_A2D_SBC_CIE_CH_MODE_MONO ? 1 : 2;
}
void mediaCodecInit(MediaPCMCallback callback, MediaFormatCallback formatCallback) {
    emitPCM = callback;
    setFormat = formatCallback;
    decoderLock = xSemaphoreCreateMutex();
    packets = xQueueCreate(24, sizeof(Packet));
    assert(decoderLock && packets);
    ESP_ERROR_CHECK((esp_err_t)esp_aac_dec_register());
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(decodeTask, "media-decode", 6144, nullptr, 4, nullptr, 1)
                   == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
void mediaCodecRegisterEndpoints(bool sbcOnly) {
    esp_a2d_mcc_t sbc = {};
    sbc.type = ESP_A2D_MCT_SBC;
    auto &s = sbc.cie.sbc_info;
    s.samp_freq = ESP_A2D_SBC_CIE_SF_44K | ESP_A2D_SBC_CIE_SF_48K;
    s.ch_mode = ESP_A2D_SBC_CIE_CH_MODE_MONO | ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL |
                ESP_A2D_SBC_CIE_CH_MODE_STEREO | ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
    s.block_len = ESP_A2D_SBC_CIE_BLOCK_LEN_4 | ESP_A2D_SBC_CIE_BLOCK_LEN_8 |
                  ESP_A2D_SBC_CIE_BLOCK_LEN_12 | ESP_A2D_SBC_CIE_BLOCK_LEN_16;
    s.num_subbands = ESP_A2D_SBC_CIE_NUM_SUBBANDS_4 | ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
    s.alloc_mthd = ESP_A2D_SBC_CIE_ALLOC_MTHD_SNR | ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
    s.min_bitpool = 2;
    // The ESP-IDF initiator compares peer capability ranges (macOS advertises 250).
    // The decoder accepts the full valid range; the source chooses its actual bitpool.
    s.max_bitpool = 250;
    // Configure the mandatory fallback first. Endpoint 0 has local preference.
    ESP_ERROR_CHECK(esp_a2d_sink_register_stream_endpoint(1, &sbc));
    esp_a2d_mcc_t aac = {};
    aac.type = ESP_A2D_MCT_M24;
    auto &a = aac.cie.m24_info;
    a.obj_type = ESP_A2D_M24_CIE_OBJ_TYPE_2_AAC_LC | ESP_A2D_M24_CIE_OBJ_TYPE_4_AAC_LC;
    a.samp_freq1 = ESP_A2D_M24_CIE_SF1_44K;
    a.samp_freq2 = ESP_A2D_M24_CIE_SF2_48K;
    a.ch = ESP_A2D_M24_CIE_CH_1 | ESP_A2D_M24_CIE_CH_2;
    a.vbr = ESP_A2D_M24_CIE_VBR_SUPPORT;
    constexpr uint32_t bitrate = 320000;
    a.br1 = (bitrate >> 16) & ESP_A2D_M24_CIE_BR1_MSK;
    a.br2 = (bitrate >> 8) & ESP_A2D_M24_CIE_BR2_MSK;
    a.br3 = bitrate & ESP_A2D_M24_CIE_BR3_MSK;
    ESP_ERROR_CHECK(esp_a2d_sink_register_stream_endpoint(0, sbcOnly ? &sbc : &aac));
    printf("CODEC preference=%s fallback=SBC\n", sbcOnly ? "SBC-test" : "AAC-LC");
}
void mediaCodecConfigure(const esp_a2d_mcc_t &mcc) {
    xSemaphoreTake(decoderLock, portMAX_DELAY);
    generation++;
    closeDecoder();
    codecType = mcc.type;
    rate = mediaCodecRate(mcc);
    channels = mediaCodecChannels(mcc);
    // No old-format packet can publish PCM after the bridge clears its ring.
    setFormat(rate.load(), channels.load());
    esp_audio_err_t error = ESP_AUDIO_ERR_FAIL;
    if (mcc.type == ESP_A2D_MCT_M24) {
        esp_aac_dec_cfg_t config = {};
        config.sample_rate = rate.load();
        config.channel = channels.load();
        config.bits_per_sample = ESP_AUDIO_BIT16;
        config.no_adts_header = true;
        config.aac_plus_enable = false;
        esp_audio_dec_cfg_t dec = {};
        dec.type = ESP_AUDIO_TYPE_AAC; dec.cfg = &config; dec.cfg_sz = sizeof(config);
        error = esp_audio_dec_open(&dec, &decoder);
    } else if (mcc.type == ESP_A2D_MCT_SBC) {
        sbcDecoder = (SbcDecoder *)calloc(1, sizeof(SbcDecoder));
        if (sbcDecoder) {
            const auto status = OI_CODEC_SBC_DecoderReset(&sbcDecoder->context, sbcDecoder->data,
                sizeof(sbcDecoder->data), channels.load(), channels.load(), FALSE, FALSE);
            error = OI_SUCCESS(status) ? ESP_AUDIO_ERR_OK : ESP_AUDIO_ERR_FAIL;
        } else error = ESP_AUDIO_ERR_MEM_LACK;
    }
    if (error != ESP_AUDIO_ERR_OK) { openErrors++; closeDecoder(); }
    printf("CODEC negotiated=%s rate=%lu channels=%lu decoder=%d heap=%lu largest=%lu\n",
           mcc.type == ESP_A2D_MCT_M24 ? "AAC-LC" : "SBC", (unsigned long)rate.load(),
           (unsigned long)channels.load(), error,
           (unsigned long)esp_get_free_heap_size(),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    xSemaphoreGive(decoderLock);
}
void mediaCodecDisconnect() {
    xSemaphoreTake(decoderLock, portMAX_DELAY);
    generation++;
    closeDecoder();
    xSemaphoreGive(decoderLock);
}
void mediaCodecStreamChanged(bool active) {
    if (active) resetSequence = true;
}
void mediaCodecReceive(esp_a2d_conn_hdl_t, esp_a2d_audio_buff_t *buffer) {
    if (!buffer) return;
    received++;
    bytes += buffer->data_len;
    Packet packet{buffer, generation.load()};
    if (!buffer->data_len || xQueueSend(packets, &packet, 0) != pdTRUE) {
        queueDrops++;
        esp_a2d_audio_buff_free(buffer);
    } else highWater(queuePeak, uxQueueMessagesWaiting(packets));
}
void mediaCodecPrintStats() {
    printf("BT_STATS {\"codec\":\"%s\",\"generation\":%lu,\"rate\":%lu,\"channels\":%lu",
           codecType == ESP_A2D_MCT_M24 ? "AAC" : codecType == ESP_A2D_MCT_SBC ? "SBC" : "none",
           (unsigned long)generation.load(), (unsigned long)rate.load(), (unsigned long)channels.load());
#define STAT(x) printf(",\"" #x "\":%lu", (unsigned long)x.load());
    STAT(received) STAT(bytes) STAT(decodedFrames) STAT(decodeCalls) STAT(queueDrops)
    STAT(stalePackets) STAT(decodeErrors) STAT(openErrors) STAT(queuePeak) STAT(decodeMaxUs)
    STAT(rtpTimestampStep) STAT(transportPackets) STAT(sequenceMissing) STAT(sequenceReordered)
    STAT(stackQueuePeak) STAT(stackQueueLimitHits)
#undef STAT
    puts("}");
}
