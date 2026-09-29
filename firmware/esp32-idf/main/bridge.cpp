/* SPDX-License-Identifier: Apache-2.0 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2s_std.h>
#include <driver/spi_master.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <nvs_flash.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_sdp_api.h>
#include "storage.h"
#include "media_codec.h"
#include <esp_timer.h>
using std::min;
using std::max;
template<typename T> static T constrain(T v, T lo, T hi) { return min(hi, max(lo, v)); }
static uint32_t millis() { return esp_timer_get_time() / 1000; }
static void delay(unsigned ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
#include <esp_timer.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_gap_bt_api.h>
#include <esp_a2dp_api.h>
#include <esp_a2dp_legacy_api.h>
#include <esp_hf_client_api.h>
#include <esp_hf_client_legacy_api.h>
#include <esp_avrc_api.h>
#include "link.h"
#include "hardware_version.h"
#include "microphone_level.h"
#include <atomic>
static_assert(sizeof(oe_link_frame) == 128, "SPI frame layout");
static AdapterStorage prefs;
static spi_device_handle_t spi;
static i2s_chan_handle_t i2sTx, i2sRx;
static std::atomic<bool> endpointsReady{false};
static bool sbcOnly;
static std::atomic<uint32_t> flags{0}, errors{0}, txFrames{0}, rxFrames{0}, rxPeak{0};
static std::atomic<uint32_t> txPeakLeft{0}, txPeakRight{0};
static std::atomic<uint32_t> sourceRate{48000}, voiceRate{16000};
static std::atomic<bool> voice{false}, testMode{false}, rxFixture{false};
static std::atomic<bool> a2dpConnected{false}, hfpConnected{false};
static std::atomic<bool> mediaStreaming{false};
static std::atomic<uint32_t> mediaDrainUntil{0};
static std::atomic<unsigned> mediaChannels{2}, microphoneVolume{8};
// Cumulative stage counters: callback PCM is decoded audio, not RF packets.
#define AUDIO_STATS(X) \
    X(a2dpFrames) X(a2dpCalls) X(hfpFrames) X(hfpCalls) X(ignoredFrames) \
    X(acceptedFrames) X(droppedFrames) X(consumedFrames) X(underflows) X(silenceFrames) \
    X(i2sTxBytes) X(i2sRxBytes) X(i2sTxErrors) X(i2sRxErrors) X(i2sTxPartial) X(i2sRxPartial) \
    X(i2sRxOverflow) X(i2sTxQueueOverflow) X(i2sDmaErrors) X(i2sTxDmaDone) X(i2sRxDmaDone) X(i2sEventBacklog) X(captureProduced) X(captureDropped) \
    X(hfpOutFrames) X(hfpOutCalls) X(hfpOutEmpty) X(micClipped) X(i2sRxPaddingErrors) X(i2sRxFixtureBad) X(micLimitedBlocks)
#define DECLARE_STAT(name) static std::atomic<uint32_t> name{0};
AUDIO_STATS(DECLARE_STAT)
#undef DECLARE_STAT
static std::atomic<uint32_t> playQueueMin{UINT32_MAX}, playQueueMax{0};
static std::atomic<uint32_t> captureQueueMin{UINT32_MAX}, captureQueueMax{0};
static std::atomic<uint32_t> micLeftMeanSquare{0}, micRightMeanSquare{0}, micMonoMeanSquare{0};
static std::atomic<uint32_t> micOutMeanSquare{0}, micLeftPeak{0}, micRightPeak{0};
static void lowWater(std::atomic<uint32_t> &value, uint32_t n) {
    uint32_t old = value.load();
    while (n < old && !value.compare_exchange_weak(old, n)) {}
}
static void highWater(std::atomic<uint32_t> &value, uint32_t n) {
    uint32_t old = value.load();
    while (n > old && !value.compare_exchange_weak(old, n)) {}
}
static uint32_t audioGeneration = 0;
static char deviceName[64] = "OpenEarable Adapter";
static char hardwareRevision[16];
static bool identityReady = false;
static uint32_t pairingUntil = 0, lastToken = 0, linkPackets = 0;
static esp_bd_addr_t lastHost = {};
static std::atomic<bool> hostAddressPending{false};
static bool hostAddressValid = false;
static uint32_t reconnectAt = 5000;
static bool tokenSeen = false;
static uint32_t lastBoot = 0, resetEpoch = 0;
static portMUX_TYPE audioMux = portMUX_INITIALIZER_UNLOCKED;
struct Frame {
    int16_t left, right;
};
struct WireFrame {
    int32_t left, right;
};
static constexpr unsigned PLAYBACK_CAPACITY = 8192;
static constexpr unsigned PLAYBACK_MASK = PLAYBACK_CAPACITY - 1;
static Frame playback[PLAYBACK_CAPACITY];
static uint32_t playRead = 0, playWrite = 0;
static int16_t capture[2048];
static uint32_t capRead = 0, capWrite = 0;
static void clearAudio() {
    portENTER_CRITICAL(&audioMux);
    playRead = playWrite = capRead = capWrite = 0;
    audioGeneration++;
    portEXIT_CRITICAL(&audioMux);
}
static void pushPCM(const uint8_t *data, uint32_t bytes, bool mono) {
    const int16_t *samples = (const int16_t *)data;
    unsigned n = bytes / (mono ? 2 : 4);
    portENTER_CRITICAL(&audioMux);
    if (n > PLAYBACK_MASK - (playWrite - playRead)) {
        errors++;
        droppedFrames += n;
        portEXIT_CRITICAL(&audioMux);
        return;
    }
    acceptedFrames += n;
    for (unsigned i = 0; i < n; i++)
        playback[playWrite++ & PLAYBACK_MASK] =
            mono ? Frame{samples[i], samples[i]} : Frame{samples[2 * i], samples[2 * i + 1]};
    portEXIT_CRITICAL(&audioMux);
}
static void a2dpData(const uint8_t *buf, uint32_t len) {
    a2dpCalls++;
    uint32_t frames = len / (mediaChannels == 1 ? 2 : 4);
    a2dpFrames += frames;
    if (!voice)
        pushPCM(buf, len, mediaChannels == 1);
    else
        ignoredFrames += frames;
}
static void a2dpFormat(unsigned rate, unsigned channels) {
    sourceRate = rate;
    mediaChannels = channels;
    clearAudio();
}
static void updateHostConnection() {
    if (a2dpConnected || hfpConnected)
        flags.fetch_or(OE_HOST_CONNECTED);
    else
        flags.fetch_and(~OE_HOST_CONNECTED);
}
static void hfpData(const uint8_t *buf, uint32_t len) {
    hfpCalls++;
    hfpFrames += len / 2;
    if (voice)
        pushPCM(buf, len, true);
    else
        ignoredFrames += len / 2;
    esp_hf_client_outgoing_data_ready();
}
static void setHeadphoneClass() {
    esp_bt_cod_t cod = {};
    cod.major = ESP_BT_COD_MAJOR_DEV_AV;
    cod.minor = 6; // Bluetooth Assigned Numbers: Headphones.
    cod.service = ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_RENDERING | ESP_BT_COD_SRVC_CAPTURING;
    ESP_ERROR_CHECK(esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL));
}
static uint32_t hfpOutgoing(uint8_t *buf, uint32_t len) {
    int16_t *out = (int16_t *)buf;
    hfpOutCalls++;
    portENTER_CRITICAL(&audioMux);
    /* HCI may request several packets in one batch. Returning a full packet
     * of artificial silence here changes the effective stream rate. */
    if (capWrite - capRead < len / 2) {
        hfpOutEmpty++;
        portEXIT_CRITICAL(&audioMux);
        return 0;
    }
    hfpOutFrames += len / 2;
    for (unsigned i = 0; i < len / 2; i++)
        out[i] = capture[capRead++ & 2047];
    portEXIT_CRITICAL(&audioMux);
    return len;
}
static void gapEvent(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *p) {
    if (event == ESP_BT_GAP_AUTH_CMPL_EVT)
        printf("BT auth=%u name=%s\n", p->auth_cmpl.stat, p->auth_cmpl.device_name);
    if (event == ESP_BT_GAP_CFM_REQ_EVT)
        esp_bt_gap_ssp_confirm_reply(p->cfm_req.bda, (int32_t)(pairingUntil - millis()) > 0);
    if (event == ESP_BT_GAP_PIN_REQ_EVT) {
        esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
        esp_bt_gap_pin_reply(p->pin_req.bda, pairingUntil > millis(), 4, pin);
    }
}
static void a2dpEvent(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *p) {
    // A2DP initialization overwrites CoD with Loudspeaker; set ours after it completes.
    if (event == ESP_A2D_PROF_STATE_EVT && p->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS) {
        setHeadphoneClass();
        mediaCodecRegisterEndpoints(sbcOnly);
    }
    if (event == ESP_A2D_SEP_REG_STATE_EVT) {
        static unsigned registered = 0;
        printf("CODEC endpoint=%u result=%u\n", p->a2d_sep_reg_stat.seid, p->a2d_sep_reg_stat.reg_state);
        if (p->a2d_sep_reg_stat.reg_state == ESP_A2D_SEP_REG_SUCCESS)
            registered |= 1u << p->a2d_sep_reg_stat.seid;
        if (registered == 3) {
            endpointsReady = true;
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        }
    }
    if (event == ESP_A2D_CONNECTION_STATE_EVT) {
        bool connected = p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED;
        if (connected) {
            setHeadphoneClass();
            a2dpConnected = true;
            memcpy(lastHost, p->conn_stat.remote_bda, sizeof(lastHost));
            hostAddressPending = true;
            if (!hfpConnected)
                esp_hf_client_connect(p->conn_stat.remote_bda);
        } else if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            a2dpConnected = false;
            mediaCodecDisconnect();
            mediaStreaming = false;
            mediaDrainUntil = 0;
            flags.fetch_and(~OE_HOST_PLAYING);
        }
        updateHostConnection();
        printf("A2DP connection=%u\n", p->conn_stat.state);
    }
    if (event == ESP_A2D_AUDIO_STATE_EVT) {
        mediaStreaming = p->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        mediaCodecStreamChanged(mediaStreaming);
        if (mediaStreaming) {
            mediaDrainUntil = 0;
            flags.fetch_or(OE_HOST_PLAYING);
        } else {
            // Drain decoded PCM and the I2S DMA tail before the DK mutes.
            mediaDrainUntil = millis() + PLAYBACK_CAPACITY * 1000 / sourceRate.load() + 80;
        }
        printf("A2DP audio=%u\n", p->audio_stat.state);
    }
    if (event == ESP_A2D_AUDIO_CFG_EVT) {
        mediaCodecConfigure(p->audio_cfg.mcc);
    }
}
static void hfpEvent(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *p) {
    if (event == ESP_HF_CLIENT_VOLUME_CONTROL_EVT) {
        printf("HFP volume target=%u value=%d\n", p->volume_control.type, p->volume_control.volume);
        if (p->volume_control.type == ESP_HF_VOLUME_CONTROL_TARGET_MIC)
            microphoneVolume = constrain(p->volume_control.volume, 0, 15);
    }
    if (event == ESP_HF_CLIENT_CONNECTION_STATE_EVT) {
        hfpConnected = p->conn_stat.state >= ESP_HF_CLIENT_CONNECTION_STATE_CONNECTED;
        updateHostConnection();
        printf("HFP connection=%u\n", p->conn_stat.state);
    }
    if (event == ESP_HF_CLIENT_AUDIO_STATE_EVT) {
        bool active = p->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED ||
                      p->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC;
        voiceRate = p->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC ? 16000 : 8000;
        voice = active;
        clearAudio();
        if (active) {
            flags.fetch_or(OE_HOST_MIC_ACTIVE | OE_HOST_CONNECTED);
            esp_hf_client_outgoing_data_ready();
        } else
            flags.fetch_and(~OE_HOST_MIC_ACTIVE);
        printf("HFP audio=%u rate=%lu\n", p->audio_stat.state,
                      (unsigned long)voiceRate.load());
    }
}
static void avrcEvent(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *p) {}
static void advertiseName() {
    esp_bt_gap_set_device_name(deviceName);
    esp_bt_eir_data_t eir = {};
    eir.include_txpower = true;
    eir.include_uuid = true;
    eir.include_name = true;
    esp_bt_gap_config_eir_data(&eir);
}
static void hostPair() {
    setHeadphoneClass();
    pairingUntil = millis() + 180000;
    flags.fetch_or(OE_HOST_PAIRABLE);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    printf("PAIR host name=%s seconds=180\n", deviceName);
}
static void resetBonds(uint32_t epoch) {
    int count = esp_bt_gap_get_bond_device_num();
    esp_bd_addr_t *devices = (esp_bd_addr_t *)malloc(count * sizeof(esp_bd_addr_t));
    if (devices && esp_bt_gap_get_bond_device_list(&count, devices) == ESP_OK) {
        for (int i = 0; i < count; i++) {
            esp_a2d_sink_disconnect(devices[i]);
            esp_hf_client_disconnect(devices[i]);
        }
        delay(300);
        for (int i = 0; i < count; i++)
            esp_bt_gap_remove_bond_device(devices[i]);
    }
    free(devices);
    resetEpoch = epoch;
    prefs.putUInt("reset_epoch", epoch);
    hostAddressValid = false;
    hostAddressPending = false;
    memset(lastHost, 0, sizeof(lastHost));
    prefs.remove("host");
    strcpy(deviceName, "OpenEarable Adapter");
    prefs.putString("name", deviceName);
    hardwareRevision[0] = 0;
    prefs.remove("hardware");
    identityReady = false;
    advertiseName();
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    pairingUntil = 0;
    flags = 0;
    a2dpConnected = false;
    hfpConnected = false;
    voice = false;
    clearAudio();
    puts("RESET host bonds cleared");
}
static void setName(const char *name) {
    if (!name[0] || !strcmp(name, deviceName))
        return;
    strlcpy(deviceName, name, sizeof(deviceName));
    advertiseName();
    prefs.putString("name", deviceName);
    printf("NAME %s\n", deviceName);
}
static void setHardwareRevision(const char *revision) {
    if (!strcmp(revision, hardwareRevision))
        return;
    strlcpy(hardwareRevision, revision, sizeof(hardwareRevision));
    prefs.putString("hardware", hardwareRevision);
    identityReady = false;
    printf("HARDWARE %s\n", hardwareRevision);
}
static void printI2SState();
static void playbackTask(void *) {
    static Frame out[480], input[512];
    static WireFrame wire[480];
    double phase = 0;
    bool primed = false;
    uint32_t phaseTest = 0, generation = 0;
    for (;;) {
        unsigned rate = voice ? voiceRate.load() : sourceRate.load();
        portENTER_CRITICAL(&audioMux);
        unsigned available = playWrite - playRead;
        if (generation != audioGeneration) {
            generation = audioGeneration;
            primed = false;
            phase = 0;
        }
        portEXIT_CRITICAL(&audioMux);
        // SBC commonly arrives in 24 ms chunks, with additional host scheduling
        // jitter. Preserve a 100 ms media reserve; speech keeps its 50 ms target.
        unsigned target = voice ? rate / 20 : rate / 10;
        unsigned prime = voice ? rate / 25 : rate * 8 / 100;
        bool draining = !voice && !mediaStreaming;
        if (!primed && (available >= prime || (draining && available))) {
            primed = true;
            phase = 0;
        }
        // Slow elastic resampling accommodates independent phone and I2S clocks.
        double correction =
            constrain(((double)available - target) / (rate * 20.0), -0.001, 0.001);
        double step = rate / 48000.0 * (1.0 + correction);
        unsigned needed = (unsigned)(phase + 479 * step) + 2;
        unsigned consumed = (unsigned)(phase + 480 * step);
        bool ready = false;
        bool padded = false;
        portENTER_CRITICAL(&audioMux);
        unsigned queued = playWrite - playRead;
        if (primed && generation == audioGeneration && (queued >= needed || (draining && queued)) &&
            needed <= 512) {
            unsigned copied = min(needed, queued);
            padded = copied < needed;
            if (padded)
                memset(input, 0, needed * sizeof(Frame));
            unsigned first = min(copied, (unsigned)(PLAYBACK_CAPACITY - (playRead & PLAYBACK_MASK)));
            memcpy(input, playback + (playRead & PLAYBACK_MASK), first * sizeof(Frame));
            memcpy(input + first, playback, (copied - first) * sizeof(Frame));
            consumed = min(consumed, queued);
            playRead += consumed;
            ready = true;
        }
        portEXIT_CRITICAL(&audioMux);
        if (ready) {
            consumedFrames += consumed;
            lowWater(playQueueMin, available);
            highWater(playQueueMax, available);
        }
        if (!ready) {
            if (primed && (mediaStreaming || voice))
                underflows++;
            if (mediaStreaming || voice)
                silenceFrames += 480;
            primed = false;
            phase = 0;
        }
        double position = phase;
        for (unsigned i = 0; i < 480; i++) {
            if (testMode) {
                int16_t x = (phaseTest++ % 48 < 24) ? 1000 : -1000;
                out[i] = {x, x};
                continue;
            }
            if (!ready) {
                out[i] = {0, 0};
                continue;
            }
            unsigned index = (unsigned)position;
            double fraction = position - index;
            Frame a = input[index], b = input[index + 1];
            out[i] = {(int16_t)(a.left + (b.left - a.left) * fraction),
                      (int16_t)(a.right + (b.right - a.right) * fraction)};
            position += step;
        }
        if (ready)
            phase = padded ? 0 : phase + 480 * step - consumed;
        uint32_t left = 0, right = 0;
        for (const Frame &frame : out) {
            left = max(left, (uint32_t)abs((int)frame.left));
            right = max(right, (uint32_t)abs((int)frame.right));
        }
        txPeakLeft = left;
        txPeakRight = right;
        for (unsigned i = 0; i < 480; i++)
            wire[i] = {(int32_t)out[i].left * 65536, (int32_t)out[i].right * 65536};
        size_t sent = 0;
        esp_err_t e = i2s_channel_write(i2sTx, wire, sizeof(wire), &sent, 1000);
        i2sTxBytes += sent;
        if (sent != sizeof(wire))
            i2sTxPartial++;
        if (e == ESP_OK)
            txFrames++;
        else {
            errors++;
            i2sTxErrors++;
            printf("I2S_TX_ERROR code=%d sent=%u\n", e, (unsigned)sent);
            printI2SState();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
static void captureTask(void *) {
    static WireFrame in[480];
    int16_t voiceBlock[160];
    MicrophoneLevel level;
    uint32_t generation = UINT32_MAX;
    int32_t sum = 0;
    unsigned phase = 0;
    // Cascaded low-pass stages suppress high-frequency energy before speech decimation.
    float low1 = 0, low2 = 0, low3 = 0;
    for (;;) {
        size_t got = 0;
        esp_err_t e = i2s_channel_read(i2sRx, in, sizeof(in), &got, 1000);
        i2sRxBytes += got;
        if (got != sizeof(in))
            i2sRxPartial++;
        if (e != ESP_OK) {
            errors++;
            i2sRxErrors++;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        portENTER_CRITICAL(&audioMux);
        uint32_t currentGeneration = audioGeneration;
        portEXIT_CRITICAL(&audioMux);
        if (currentGeneration != generation) {
            generation = currentGeneration;
            sum = 0; phase = 0; low1 = low2 = low3 = 0;
            level.reset();
        }
        unsigned rate = voiceRate;
        unsigned voiceCount = 0;
        unsigned factor = 48000 / rate;
        uint32_t peak = 0, leftPeak = 0, rightPeak = 0, outCount = 0;
        uint64_t leftSquares = 0, rightSquares = 0, monoSquares = 0, outSquares = 0;
        const float alpha = rate == 16000 ? 0.45f : 0.28f;
        for (unsigned i = 0; i < got / sizeof(WireFrame); i++) {
            if ((in[i].left & 0xffff) || (in[i].right & 0xffff))
                i2sRxPaddingErrors++;
            int32_t left = in[i].left >> 16, right = in[i].right >> 16;
            if (rxFixture && (abs(left) != 1000 || abs(right) != 1000))
                i2sRxFixtureBad++;
            int32_t mono = (left + right) / 2;
            leftSquares += (int64_t)left * left;
            rightSquares += (int64_t)right * right;
            monoSquares += (int64_t)mono * mono;
            leftPeak = max(leftPeak, (uint32_t)abs(left));
            rightPeak = max(rightPeak, (uint32_t)abs(right));
            peak = max(peak, (uint32_t)abs(mono));
            low1 += alpha * (mono - low1);
            low2 += alpha * (low1 - low2);
            low3 += alpha * (low2 - low3);
            sum += (int32_t)low3;
            if (++phase >= factor) {
                int16_t sample = sum / (int32_t)phase;
                outSquares += (int32_t)sample * sample;
                outCount++;
                if (abs((int)sample) >= 32767)
                    micClipped++;
                sum = 0;
                phase = 0;
                if (voice && voiceCount < 160)
                    voiceBlock[voiceCount++] = sample;
            }
        }
        if (voiceCount) {
            if (level.process(voiceBlock, voiceCount, rate,
                              MicrophoneLevel::gainForVolume(microphoneVolume.load())))
                micLimitedBlocks++;
            outSquares = 0;
            for (unsigned i = 0; i < voiceCount; i++)
                outSquares += (int32_t)voiceBlock[i] * voiceBlock[i];
            outCount = voiceCount;
            portENTER_CRITICAL(&audioMux);
            if (generation == audioGeneration && voice) {
                captureProduced += voiceCount;
                for (unsigned i = 0; i < voiceCount; i++) {
                    if (capWrite - capRead < 2047)
                        capture[capWrite++ & 2047] = voiceBlock[i];
                    else {
                        errors++;
                        captureDropped++;
                    }
                }
            }
            portEXIT_CRITICAL(&audioMux);
        }
        unsigned n = got / sizeof(WireFrame);
        if (n) {
            micLeftMeanSquare = leftSquares / n;
            micRightMeanSquare = rightSquares / n;
            micMonoMeanSquare = monoSquares / n;
        }
        micOutMeanSquare = outCount ? outSquares / outCount : 0;
        micLeftPeak = leftPeak;
        micRightPeak = rightPeak;
        if (voice) {
            portENTER_CRITICAL(&audioMux);
            uint32_t queued = capWrite - capRead;
            portEXIT_CRITICAL(&audioMux);
            lowWater(captureQueueMin, queued);
            highWater(captureQueueMax, queued);
        }
        rxPeak = peak;
        rxFrames++;
        if (voice)
            esp_hf_client_outgoing_data_ready();
    }
}
static void printI2SState() {
    puts("I2S full-duplex slave 48000Hz 32-bit slots, DOUT=5mA");
}
static void printAudioStats() {
#define EXTRA_STATS(X) \
    X(playQueueMin) X(playQueueMax) X(captureQueueMin) X(captureQueueMax) \
    X(micLeftMeanSquare) X(micRightMeanSquare) X(micMonoMeanSquare) \
    X(micOutMeanSquare) X(micLeftPeak) X(micRightPeak)
#define FIELD(name) uint32_t name;
    struct Snapshot { AUDIO_STATS(FIELD) EXTRA_STATS(FIELD) } snapshot;
#undef FIELD
    uint32_t playQueued, capQueued, generation;
    // Take one short snapshot before serial output, which can take >100 ms.
    portENTER_CRITICAL(&audioMux);
#define COPY(name) snapshot.name = name.load();
    AUDIO_STATS(COPY) EXTRA_STATS(COPY)
#undef COPY
    playQueued = playWrite - playRead;
    capQueued = capWrite - capRead;
    generation = audioGeneration;
    portEXIT_CRITICAL(&audioMux);
    char line[2048];
    size_t used = snprintf(line, sizeof(line),
        "AUDIO_STATS {\"ms\":%lu,\"generation\":%lu,\"voice\":%u,\"sourceRate\":%lu,\"voiceRate\":%lu",
        (unsigned long)millis(), (unsigned long)generation, voice.load(),
        (unsigned long)sourceRate.load(), (unsigned long)voiceRate.load());
#define PRINT_STAT(name) used += snprintf(line + used, sizeof(line) - used, ",\"" #name "\":%lu", (unsigned long)snapshot.name);
    AUDIO_STATS(PRINT_STAT) EXTRA_STATS(PRINT_STAT)
#undef PRINT_STAT
#undef EXTRA_STATS
    snprintf(line + used, sizeof(line) - used,
             ",\"playQueued\":%lu,\"captureQueued\":%lu}\n",
             (unsigned long)playQueued, (unsigned long)capQueued);
    fputs(line, stdout);
    mediaCodecPrintStats();
}
static bool IRAM_ATTR txDone(i2s_chan_handle_t, i2s_event_data_t *, void *) {
    i2sTxDmaDone++; return false;
}
static bool IRAM_ATTR rxDone(i2s_chan_handle_t, i2s_event_data_t *, void *) {
    i2sRxDmaDone++; return false;
}
static bool IRAM_ATTR txOverflow(i2s_chan_handle_t, i2s_event_data_t *, void *) {
    i2sTxQueueOverflow++; return false;
}
static bool IRAM_ATTR rxOverflow(i2s_chan_handle_t, i2s_event_data_t *, void *) {
    i2sRxOverflow++; return false;
}
static void initI2S() {
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    channel.dma_desc_num = 6;
    channel.dma_frame_num = 160;
    channel.auto_clear_after_cb = true;
    ESP_ERROR_CHECK(i2s_new_channel(&channel, &i2sTx, &i2sRx));
    i2s_std_config_t config = {};
    config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000);
    config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    config.gpio_cfg.bclk = GPIO_NUM_26;
    config.gpio_cfg.ws = GPIO_NUM_25;
    config.gpio_cfg.dout = GPIO_NUM_22;
    config.gpio_cfg.din = GPIO_NUM_35;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2sTx, &config));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2sRx, &config));
    i2s_event_callbacks_t txCallbacks = {}, rxCallbacks = {};
    txCallbacks.on_sent = txDone;
    txCallbacks.on_send_q_ovf = txOverflow;
    rxCallbacks.on_recv = rxDone;
    rxCallbacks.on_recv_q_ovf = rxOverflow;
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(i2sTx, &txCallbacks, nullptr));
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(i2sRx, &rxCallbacks, nullptr));
    ESP_ERROR_CHECK(gpio_set_drive_capability(GPIO_NUM_22, GPIO_DRIVE_CAP_0));
    ESP_ERROR_CHECK(i2s_channel_enable(i2sTx));
    ESP_ERROR_CHECK(i2s_channel_enable(i2sRx));
    xTaskCreatePinnedToCore(playbackTask, "pcm-out", 4096, nullptr, 5, nullptr, 1);
    xTaskCreatePinnedToCore(captureTask, "pcm-in", 4096, nullptr, 5, nullptr, 1);
}
static std::atomic<bool> sdpReady{false}, identityPending{false};
static void sdpEvent(esp_sdp_cb_event_t event, esp_sdp_cb_param_t *p) {
    if (event == ESP_SDP_INIT_EVT) sdpReady = p->init.status == ESP_SDP_SUCCESS;
    if (event == ESP_SDP_CREATE_RECORD_COMP_EVT) {
        identityReady = p->create_record.status == ESP_SDP_SUCCESS;
        identityPending = false;
        printf("IDENTITY registered=%u hardware=%s\n", identityReady, hardwareRevision);
    }
}
static void advertisePrototypeIdentity(uint16_t hardwareVersion) {
    esp_bluetooth_sdp_record_t record = {};
    record.dip.hdr.type = ESP_SDP_TYPE_DIP_SERVER;
    record.dip.hdr.rfcomm_channel_number = -1;
    record.dip.hdr.l2cap_psm = -1;
    record.dip.vendor = 0xffff;
    record.dip.vendor_id_source = ESP_SDP_VENDOR_ID_SRC_BT;
    record.dip.product = 1;
    record.dip.version = hardwareVersion;
    record.dip.primary_record = true;
    identityPending = esp_sdp_create_record(&record) == ESP_OK;
}
void setup() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(nvs_flash_init());
    delay(300);
    puts("OpenEarable Classic adapter boot");
    prefs.begin();
    sbcOnly = prefs.getUInt("codec_sbc", 0) != 0;
    resetEpoch = prefs.getUInt("reset_epoch", 0);
    strlcpy(deviceName, prefs.getString("name", "OpenEarable Adapter").c_str(), sizeof(deviceName));
    strlcpy(hardwareRevision, prefs.getString("hardware", "").c_str(), sizeof(hardwareRevision));
    gpio_set_direction(GPIO_NUM_33, GPIO_MODE_INPUT);
    gpio_set_direction(GPIO_NUM_27, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_27, 1);
    spi_bus_config_t bus = {};
    bus.mosi_io_num = 23; bus.miso_io_num = 19; bus.sclk_io_num = 18;
    bus.quadwp_io_num = -1; bus.quadhd_io_num = -1;
    bus.max_transfer_sz = sizeof(oe_link_frame);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = 1000000; dev.spics_io_num = -1; dev.queue_size = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev, &spi));
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    cfg.mode = ESP_BT_MODE_CLASSIC_BT;
    ESP_ERROR_CHECK(esp_bt_controller_init(&cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_sdp_register_callback(sdpEvent));
    ESP_ERROR_CHECK(esp_sdp_init());
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gapEvent));
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(deviceName));
    ESP_ERROR_CHECK(esp_avrc_ct_init());
    ESP_ERROR_CHECK(esp_avrc_ct_register_callback(avrcEvent));
    ESP_ERROR_CHECK(esp_hf_client_register_callback(hfpEvent));
    ESP_ERROR_CHECK(esp_hf_client_init());
    ESP_ERROR_CHECK(esp_hf_client_register_data_callback(hfpData, hfpOutgoing));
    ESP_ERROR_CHECK(esp_a2d_register_callback(a2dpEvent));
    mediaCodecInit(a2dpData, a2dpFormat);
    ESP_ERROR_CHECK(esp_a2d_sink_register_audio_data_callback(mediaCodecReceive));
    ESP_ERROR_CHECK(esp_a2d_sink_init());
    hostAddressValid = prefs.getBytes("host", lastHost, sizeof(lastHost)) == sizeof(lastHost);
    if (!hostAddressValid && esp_bt_gap_get_bond_device_num() == 1) {
        int count = 1;
        hostAddressValid = esp_bt_gap_get_bond_device_list(&count, &lastHost) == ESP_OK;
    }
    advertiseName();
    initI2S();
    const uint8_t *address = esp_bt_dev_get_address();
    printf("READY name=%s heap=%u BT=%02x:%02x:%02x:%02x:%02x:%02x\n", deviceName,
                  (unsigned)esp_get_free_heap_size(), address[0], address[1], address[2], address[3], address[4],
                  address[5]);
}
void loop() {
    static uint32_t seq = 0, report = 0;
    static std::string command;
    uint32_t drainUntil = mediaDrainUntil;
    if (drainUntil && !mediaStreaming && (int32_t)(millis() - drainUntil) >= 0) {
        flags.fetch_and(~OE_HOST_PLAYING);
        mediaDrainUntil = 0;
    }
    // Publish after asynchronous Bluedroid initialization has completed its
    // database setup, and before the first automatic host reconnect.
    if (!identityReady && !identityPending && sdpReady && millis() >= 3000) {
        uint16_t version = 0;
        oe_hardware_version_bcd(hardwareRevision, &version);
        advertisePrototypeIdentity(version);
        setHeadphoneClass();
        advertiseName();
    }
    if (hostAddressPending.exchange(false)) {
        hostAddressValid = true;
        prefs.putBytes("host", lastHost, sizeof(lastHost));
    }
    if (endpointsReady && hostAddressValid && !a2dpConnected && !pairingUntil &&
        (int32_t)(millis() - reconnectAt) >= 0) {
        static bool tryA2dpNext = false;
        reconnectAt = millis() + 15000;
        // Give the source a chance to discover AAC: macOS advertises SBC-only
        // source endpoints when the sink initiates AVDTP. HFP brings up the
        // bonded host, whose profile manager can then initiate A2DP itself.
        if (!hfpConnected && !tryA2dpNext) {
            esp_hf_client_connect(lastHost);
            tryA2dpNext = true;
        } else {
            esp_a2d_sink_connect(lastHost);
            tryA2dpNext = false;
        }
    }
    char c;
    while (uart_read_bytes(UART_NUM_0, &c, 1, 0) == 1) {
        if (c == '\n' || c == '\r') {
            if (command == "PAIR")
                hostPair();
            else if (command == "TEST ON")
                testMode = true;
            else if (command == "TEST OFF")
                testMode = false;
            else if (command == "STATS")
                printAudioStats();
            else if (command.rfind("MIC VOLUME ", 0) == 0) {
                microphoneVolume = constrain(atoi(command.c_str() + 11), 0, 15);
                printf("Microphone volume=%u\n", microphoneVolume.load());
            }
            else if (command == "DRIVE LOW") {
                gpio_set_drive_capability(GPIO_NUM_22, GPIO_DRIVE_CAP_0);
                puts("DOUT drive=5mA");
            }
            else if (command == "DRIVE HIGH") {
                gpio_set_drive_capability(GPIO_NUM_22, GPIO_DRIVE_CAP_2);
                puts("DOUT drive=20mA");
            }
            else if (command == "PULL OFF") {
                gpio_set_pull_mode(GPIO_NUM_25, GPIO_FLOATING);
                gpio_set_pull_mode(GPIO_NUM_26, GPIO_FLOATING);
                puts("Clock pullups disabled");
            }
            else if (command == "RXCHECK ON")
                rxFixture = true;
            else if (command == "RXCHECK OFF")
                rxFixture = false;
            else if (command == "I2S")
                printI2SState();
            else if (command == "CODEC SBC" || command == "CODEC AUTO") {
                // Diagnostic override survives reboot; AUTO is the shipped/default policy.
                prefs.putUInt("codec_sbc", command == "CODEC SBC");
                puts("CODEC policy saved, restarting");
                delay(100);
                esp_restart();
            }
            command = "";
        } else if (command.length() < 80)
            command += c;
    }
    if (gpio_get_level(GPIO_NUM_33)) {
        oe_link_frame tx = {}, rx = {};
        tx.sequence = ++seq;
        tx.flags = flags;
        tx.pcm_frames = txFrames;
        tx.peak = rxPeak;
        tx.errors = errors;
        oe_link_seal(&tx);
        spi_transaction_t transaction = {};
        transaction.length = sizeof(tx) * 8;
        transaction.tx_buffer = &tx;
        transaction.rx_buffer = &rx;
        gpio_set_level(GPIO_NUM_27, 0);
        esp_err_t transfer = spi_device_polling_transmit(spi, &transaction);
        gpio_set_level(GPIO_NUM_27, 1);
        if (transfer != ESP_OK) { errors++; delay(20); return; }
        if (oe_link_valid(&rx)) {
            linkPackets++;
            rx.name[sizeof(rx.name) - 1] = 0;
            rx.hardware_revision[sizeof(rx.hardware_revision) - 1] = 0;
            if (rx.reset_epoch != resetEpoch)
                resetBonds(rx.reset_epoch);
            else if (rx.flags & OE_NAME_VALID) {
                setName(rx.name);
                setHardwareRevision(rx.hardware_revision);
            }
            if (rx.boot_id != lastBoot) {
                lastBoot = rx.boot_id;
                tokenSeen = false;
            }
            if (!tokenSeen) {
                lastToken = rx.pairing_token;
                tokenSeen = true;
            }
            if (lastToken != rx.pairing_token) {
                lastToken = rx.pairing_token;
                hostPair();
            }
        } else
            errors++;
    }
    if (pairingUntil && (int32_t)(millis() - pairingUntil) >= 0) {
        pairingUntil = 0;
        flags.fetch_and(~OE_HOST_PAIRABLE);
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    }
    if (millis() - report >= 5000) {
        report = millis();
        // BTA registers profiles asynchronously after its init callback. Check
        // the final value as well, before a host can cache the speaker default.
        esp_bt_cod_t cod = {};
        if (esp_bt_gap_get_cod(&cod) == ESP_OK) {
            if (cod.major != ESP_BT_COD_MAJOR_DEV_AV || cod.minor != 6)
                setHeadphoneClass();
            printf("CLASS major=%u minor=%u service=%u\n", cod.major, cod.minor,
                          cod.service);
        }
        printf("STATUS link=%lu tx=%lu rx=%lu peak=%lu errors=%lu flags=%lu heap=%u\n",
                      (unsigned long)linkPackets, (unsigned long)txFrames.load(),
                      (unsigned long)rxFrames.load(), (unsigned long)rxPeak.load(),
                      (unsigned long)errors.load(), (unsigned long)flags.load(), (unsigned)esp_get_free_heap_size());
        printf("PCM out=%lu,%lu\n", (unsigned long)txPeakLeft.load(),
                      (unsigned long)txPeakRight.load());
        printAudioStats();
    }
    delay(20);
}

extern "C" void app_main() {
    setup();
    for (;;) loop();
}
