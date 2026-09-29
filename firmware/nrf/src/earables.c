/* SPDX-License-Identifier: Apache-2.0
 * BAP sequencing follows Zephyr's bap_unicast_client sample (Nordic, 2021-2024).
 */
#include "adapter.h"
#include "proxy.h"
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/bap_lc3_preset.h>
#include <zephyr/bluetooth/audio/csip.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/bluetooth/hci.h>
#include <bluetooth/hci_vs_sdc.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <lc3.h>
#define PEERS 2
#define OCTETS 120
#define ST_CONFIG 1
#define ST_QOS 2
#define ST_ENABLE 4
#define ST_CONNECTED 8
#define ST_STARTED 16
struct audio_stream {
    struct bt_bap_stream bap;
    atomic_t state;
    uint16_t sequence;
};
struct audio_packet {
    uint8_t peer, len;
    uint16_t seq;
    uint8_t data[155];
};
atomic_t audio_tx_packets, audio_tx_errors, audio_rx_packets, audio_rx_concealed;
atomic_t audio_rx_queue_drops, audio_rx_invalid, audio_rx_startup_discarded;
atomic_t audio_peer_rx[PEERS], audio_peer_invalid[PEERS];
atomic_t audio_tx_sync_frames, audio_tx_sync_errors;
atomic_t audio_tx_same_event, audio_tx_skipped_events;
atomic_t audio_rx_late[PEERS], audio_rx_missing[PEERS], audio_rx_queue_depth[PEERS];
static uint32_t last_tx_timestamp;
static bool tx_timestamp_valid;
struct peer {
    struct bt_conn *conn;
    volatile bool connected, secure, discovered;
    volatile int error;
    unsigned side;
    char name[64];
    char hardware_revision[16];
    struct bt_csip_set_coordinator_set_info set;
    struct bt_bap_ep *sink, *source;
    struct audio_stream tx, rx;
    struct bt_bap_lc3_preset preset, capture_preset;
    lc3_encoder_mem_48k_t enc_mem;
    lc3_decoder_mem_48k_t dec_mem;
    lc3_encoder_t encoder;
    lc3_decoder_t decoder;
    bool capture_primed, packet_pending;
    uint16_t capture_sequence;
    struct audio_packet pending;
};
static struct peer peers[PEERS];
static struct bt_bap_unicast_group *group;
static atomic_t pairing, streaming, request_mode, skip_saved_first;
static K_MUTEX_DEFINE(audio_mutex);
struct saved_pair {
    uint8_t count;
    bt_addr_le_t address[2];
    char name[64];
    uint32_t reset_epoch;
    char hardware_revision[16];
};
static struct saved_pair saved;
static int load_pair(const char *key, size_t len, settings_read_cb read_cb, void *arg) {
    /* Accept the old saved layout so an adapter update retains its bonds. */
    if (strcmp(key, "pair") ||
        (len != sizeof(saved) && len != offsetof(struct saved_pair, hardware_revision)))
        return -ENOENT;
    int n = read_cb(arg, &saved, len);
    if (n != len || saved.count > 2) {
        memset(&saved, 0, sizeof(saved));
        return -EINVAL;
    }
    saved.name[63] = 0;
    saved.hardware_revision[15] = 0;
    k_mutex_lock(&name_mutex, K_FOREVER);
    atomic_set(&reset_epoch, saved.reset_epoch);
    if (saved.count) {
        memcpy(pair_name, saved.name, sizeof(pair_name));
        memcpy(pair_hardware_revision, saved.hardware_revision, sizeof(pair_hardware_revision));
        atomic_or(&ear_flags, OE_NAME_VALID);
    }
    k_mutex_unlock(&name_mutex);
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(adapter, "adapter", NULL, load_pair, NULL, NULL);
static K_SEM_DEFINE(pair_request, 0, 1);
static struct k_spinlock command_lock;
static void request_pairing(int mode, bool only_if_idle) {
    k_spinlock_key_t key = k_spin_lock(&command_lock);
    if (!only_if_idle || !k_sem_count_get(&pair_request)) {
        atomic_set(&request_mode, mode);
        k_sem_give(&pair_request);
    }
    k_spin_unlock(&command_lock, key);
}
struct candidate {
    bt_addr_le_t addr;
    char name[64];
    uint8_t rsi[BT_CSIP_RSI_SIZE];
    bool has_rsi;
};
K_MSGQ_DEFINE(candidates, sizeof(struct candidate), 4, 4);
K_MSGQ_DEFINE(capture_left, sizeof(struct audio_packet), 16, 4);
K_MSGQ_DEFINE(capture_right, sizeof(struct audio_packet), 16, 4);
static struct k_msgq *const capture_packets[] = {&capture_left, &capture_right};
NET_BUF_POOL_FIXED_DEFINE(iso_pool, 8, BT_ISO_SDU_BUF_SIZE(OCTETS),
                          CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);
static struct peer *find(struct bt_conn *conn) {
    for (unsigned i = 0; i < PEERS; i++)
        if (peers[i].conn == conn)
            return &peers[i];
    return NULL;
}
static struct audio_stream *audio(struct bt_bap_stream *s) {
    return CONTAINER_OF(s, struct audio_stream, bap);
}
static void connected(struct bt_conn *conn, uint8_t err) {
    struct peer *p = find(conn);
    if (!p)
        return;
    p->error = err ? -ECONNREFUSED : 0;
    p->connected = !err;
    printk("EAR connected=%d error=%u\n", (int)(p - peers), err);
    if (!err) {
        int e = bt_conn_set_security(conn, BT_SECURITY_L2);
        if (e)
            p->error = e;
    }
}
static void disconnected(struct bt_conn *conn, uint8_t reason) {
    struct peer *p = find(conn);
    if (!p)
        return;
    p->connected = false;
    p->secure = false;
    p->error = -ENOTCONN;
    atomic_set(&streaming, 0);
    atomic_and(&ear_flags, OE_NAME_VALID);
    printk("EAR disconnected=%d reason=%u\n", (int)(p - peers), reason);
}
static void security(struct bt_conn *conn, bt_security_t level, enum bt_security_err err) {
    struct peer *p = find(conn);
    if (!p)
        return;
    p->secure = !err && level >= BT_SECURITY_L2;
    p->error = err ? -EACCES : 0;
    printk("EAR security=%d level=%u error=%u\n", (int)(p - peers), level, err);
}
static bool parameter_request(struct bt_conn *conn, struct bt_le_conn_param *param) {
    if (!find(conn))
        return true;
    /* Preserve the BAP setup interval. The earables' sensor-oriented adaptive
     * policy requests 10 ms ACL events, competing with two bidirectional CISes.
     * Host-to-proxy connection parameters remain under the host's control. */
    printk("EAR retaining BAP interval; requested=%u..%u\n", param->interval_min,
           param->interval_max);
    return false;
}
static void parameters_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
                               uint16_t timeout) {
    struct peer *p = find(conn);
    if (p)
        printk("EAR interval=%u units latency=%u timeout=%u peer=%u\n", interval, latency, timeout,
               (unsigned)(p - peers));
}
BT_CONN_CB_DEFINE(callbacks) = {.connected = connected,
                                .disconnected = disconnected,
                                .security_changed = security,
                                .le_param_req = parameter_request,
                                .le_param_updated = parameters_updated};
static void configured(struct bt_bap_stream *s, const struct bt_bap_qos_cfg_pref *pref) {
    atomic_or(&audio(s)->state, ST_CONFIG);
}
static void qos(struct bt_bap_stream *s) { atomic_or(&audio(s)->state, ST_QOS); }
static void enabled(struct bt_bap_stream *s) { atomic_or(&audio(s)->state, ST_ENABLE); }
static void stream_connected(struct bt_bap_stream *s) { atomic_or(&audio(s)->state, ST_CONNECTED); }
static void started(struct bt_bap_stream *s) {
    atomic_or(&audio(s)->state, ST_STARTED);
    audio(s)->sequence = 0;
}
static void stopped(struct bt_bap_stream *s, uint8_t reason) {
    atomic_and(&audio(s)->state, ~ST_STARTED);
    atomic_set(&streaming, 0);
    atomic_and(&ear_flags, OE_NAME_VALID);
}
static void released(struct bt_bap_stream *s) { atomic_set(&audio(s)->state, 0); }
static void recv(struct bt_bap_stream *s, const struct bt_iso_recv_info *info,
                 struct net_buf *buf) {
    struct peer *p = find(s->conn);
    if (!p || buf->len > 155)
        return;
    struct audio_packet packet = {.peer = p - peers,
                                  .len = (info->flags & BT_ISO_FLAGS_VALID) ? buf->len : 0,
                                  .seq = info->seq_num};
    memcpy(packet.data, buf->data, packet.len);
    atomic_inc(&audio_rx_packets);
    atomic_inc(&audio_peer_rx[packet.peer]);
    if (!packet.len) {
        atomic_inc(&audio_rx_invalid);
        atomic_inc(&audio_peer_invalid[packet.peer]);
    }
    if (!atomic_get(&streaming)) {
        // The first CIS starts before the second. These setup packets have no
        // presentation consumer yet; record them separately from active drops.
        atomic_inc(&audio_rx_startup_discarded);
        return;
    }
    if (k_msgq_put(capture_packets[packet.peer], &packet, K_NO_WAIT)) {
        atomic_inc(&audio_rx_queue_drops);
        atomic_inc(&link_errors);
    }
}
static struct bt_bap_stream_ops ops = {.configured = configured,
                                       .qos_set = qos,
                                       .enabled = enabled,
                                       .connected = stream_connected,
                                       .started = started,
                                       .stopped = stopped,
                                       .released = released,
                                       .recv = recv};
static void location(struct bt_conn *conn, enum bt_audio_dir dir, enum bt_audio_location loc) {
    struct peer *p = find(conn);
    if (p && dir == BT_AUDIO_DIR_SINK)
        p->side = (loc & BT_AUDIO_LOCATION_FRONT_RIGHT) ? 1 : 0;
    printk("EAR location=%x dir=%d\n", loc, dir);
}
static void endpoint(struct bt_conn *conn, enum bt_audio_dir dir, struct bt_bap_ep *ep) {
    struct peer *p = find(conn);
    if (!p)
        return;
    if (dir == BT_AUDIO_DIR_SINK)
        p->sink = ep;
    else
        p->source = ep;
}
static void discovered(struct bt_conn *conn, int err, enum bt_audio_dir dir) {
    struct peer *p = find(conn);
    if (!p)
        return;
    p->error = err;
    p->discovered = true;
    printk("EAR discovered=%d dir=%d error=%d\n", (int)(p - peers), dir, err);
}
static void response(struct bt_bap_stream *s, enum bt_bap_ascs_rsp_code code,
                     enum bt_bap_ascs_reason reason) {
    if (code) {
        struct peer *p = find(s->conn);
        if (p)
            p->error = -EPROTO;
        printk("ASE rejected code=%u reason=%u\n", code, reason);
    }
}
static struct bt_bap_unicast_client_cb client = {.location = location,
                                                 .endpoint = endpoint,
                                                 .discover = discovered,
                                                 .config = response,
                                                 .qos = response,
                                                 .enable = response,
                                                 .start = response};
static void set_discovered(struct bt_conn *conn,
                           const struct bt_csip_set_coordinator_set_member *member, int err,
                           size_t count) {
    struct peer *p = find(conn);
    if (!p)
        return;
    p->error = err ? err : (!count ? -ENOTSUP : 0);
    if (!p->error)
        p->set = member->insts[0].info;
    p->discovered = true;
    printk("EAR set rank=%u size=%u error=%d\n", p->set.rank, p->set.set_size, p->error);
}
static struct bt_csip_set_coordinator_cb set_callbacks = {.discover = set_discovered};
static bool parse_name(struct bt_data *data, void *arg) {
    struct candidate *c = arg;
    if (data->type == BT_DATA_CSIS_RSI && data->data_len == sizeof(c->rsi)) {
        memcpy(c->rsi, data->data, sizeof(c->rsi));
        c->has_rsi = true;
    }
    if (data->type != BT_DATA_NAME_COMPLETE && data->type != BT_DATA_NAME_SHORTENED)
        return true;
    size_t n = MIN(data->data_len, sizeof(c->name) - 1);
    memcpy(c->name, data->data, n);
    c->name[n] = 0;
    return true;
}
static void scan(const struct bt_le_scan_recv_info *info, struct net_buf_simple *ad) {
    const bt_addr_le_t *addr = info->addr;
    if (!(info->adv_props & BT_GAP_ADV_PROP_CONNECTABLE))
        return;
    struct candidate c = {.addr = *addr};
    bt_data_parse(ad, parse_name, &c);
    if (strncmp(c.name, "OpenEarable", 11) != 0)
        return;
    // Set membership comes from CSIP, not a shared display name. Individual
    // left and right devices may retain different GAP names.
    if (peers[0].discovered && c.has_rsi) {
        struct bt_data set_id = BT_CSIP_DATA_RSI(c.rsi);
        if (!bt_csip_set_coordinator_is_set_member(peers[0].set.sirk, &set_id))
            return;
    }
    /* A replace request must not immediately select the old, still-powered
     * set. After finding a new first member, its matching mate may be saved. */
    if (atomic_get(&skip_saved_first))
        for (unsigned i = 0; i < saved.count; i++)
            if (bt_addr_le_eq(addr, &saved.address[i]))
                return;
    if (!atomic_get(&pairing) && strcmp(c.name, pair_name) != 0)
        return;
    for (unsigned i = 0; i < PEERS; i++)
        if (peers[i].conn && bt_addr_le_eq(addr, bt_conn_get_dst(peers[i].conn)))
            return;
    k_msgq_put(&candidates, &c, K_NO_WAIT);
}
static struct bt_le_scan_cb scan_callbacks = {.recv = scan};
static int wait_peer(struct peer *p, volatile bool *flag) {
    int64_t end = k_uptime_get() + 15000;
    while (!*flag && !p->error && k_uptime_get() < end) {
        if (k_sem_count_get(&pair_request))
            return -ECANCELED;
        k_sleep(K_MSEC(10));
    }
    return p->error ? p->error : (*flag ? 0 : -ETIMEDOUT);
}
static int wait_stream(struct peer *p, struct audio_stream *s, unsigned flag) {
    int64_t end = k_uptime_get() + 7000;
    while (!(atomic_get(&s->state) & flag) && !p->error && k_uptime_get() < end) {
        if (k_sem_count_get(&pair_request))
            return -ECANCELED;
        k_sleep(K_MSEC(5));
    }
    return p->error ? p->error : ((atomic_get(&s->state) & flag) ? 0 : -ETIMEDOUT);
}
static int connect_peer(struct peer *p, struct candidate *c) {
    memcpy(p->name, c->name, sizeof(p->name));
    p->error = 0;
    struct bt_conn_le_create_param create = *BT_CONN_LE_CREATE_CONN;
    create.timeout = 1000; // 10 seconds, in 10 ms units.
    int e = bt_conn_le_create(&c->addr, &create, BT_BAP_CONN_PARAM_RELAXED, &p->conn);
    if (e)
        return e;
    e = wait_peer(p, &p->connected);
    if (e)
        return e;
    e = wait_peer(p, &p->secure);
    if (e)
        return e;
    e = proxy_attach(p - peers, p->conn, p->name, sizeof(p->name));
    if (e)
        return e;
    const struct bt_uuid_128 hardware_uuid =
        BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x45622512, 0x6468, 0x465a, 0xb141, 0x0b9b0f96b468));
    e = proxy_read_uuid(p->conn, &hardware_uuid.uuid, p->hardware_revision,
                        sizeof(p->hardware_revision) - 1);
    if (e < 0) {
        p->hardware_revision[0] = 0;
        printk("EAR hardware revision unavailable=%d\n", e);
    } else {
        p->hardware_revision[e] = 0;
        printk("EAR hardware revision=%s\n", p->hardware_revision);
    }
    p->discovered = false;
    e = bt_csip_set_coordinator_discover(p->conn);
    if (e)
        return e;
    e = wait_peer(p, &p->discovered);
    if (e)
        return e;
    if (p != peers && memcmp(p->set.sirk, peers[0].set.sirk, BT_CSIP_SIRK_SIZE))
        return -EXDEV;
    /* Classic hosts already attenuate PCM when absolute-volume support is not
     * advertised. Leave no additional attenuation in the LE Audio renderer. */
    e = proxy_set_render_volume(p - peers, p->conn, 255);
    if (e)
        return e;
    for (int dir = BT_AUDIO_DIR_SINK; dir <= BT_AUDIO_DIR_SOURCE; dir++) {
        p->discovered = false;
        e = bt_bap_unicast_client_discover(p->conn, dir);
        if (e)
            return e;
        e = wait_peer(p, &p->discovered);
        if (e)
            return e;
    }
    if (!p->sink || !p->source)
        return -ENOTSUP;
    p->preset = (struct bt_bap_lc3_preset)BT_BAP_LC3_UNICAST_PRESET_48_4_2(
        p->side ? BT_AUDIO_LOCATION_FRONT_RIGHT : BT_AUDIO_LOCATION_FRONT_LEFT,
        BT_AUDIO_CONTEXT_TYPE_MEDIA);
    /* HFP exposes at most 16 kHz capture. Negotiate that rate with the existing
     * earable SRC instead of spending radio time on discarded high frequencies. */
    p->capture_preset = (struct bt_bap_lc3_preset)BT_BAP_LC3_UNICAST_PRESET_16_2_2(
        p->side ? BT_AUDIO_LOCATION_FRONT_RIGHT : BT_AUDIO_LOCATION_FRONT_LEFT,
        BT_AUDIO_CONTEXT_TYPE_MEDIA);
    /* High-reliability presets allow retransmissions across transient RF
     * collisions while keeping the existing codec rates and earable firmware. */
    p->encoder = lc3_setup_encoder(10000, 48000, 48000, &p->enc_mem);
    p->decoder = lc3_setup_decoder(10000, 16000, 48000, &p->dec_mem);
    if (!p->encoder || !p->decoder)
        return -ENOMEM;
    e = bt_bap_stream_config(p->conn, &p->tx.bap, p->sink, &p->preset.codec_cfg);
    if (e)
        return e;
    e = wait_stream(p, &p->tx, ST_CONFIG);
    if (e)
        return e;
    e = bt_bap_stream_config(p->conn, &p->rx.bap, p->source, &p->capture_preset.codec_cfg);
    if (e)
        return e;
    return wait_stream(p, &p->rx, ST_CONFIG);
}
static int start(unsigned n) {
    struct bt_bap_unicast_group_stream_param tx[PEERS], rx[PEERS];
    struct bt_bap_unicast_group_stream_pair_param pairs[PEERS];
    for (unsigned i = 0; i < n; i++) {
        tx[i] = (struct bt_bap_unicast_group_stream_param){.stream = &peers[i].tx.bap,
                                                           .qos = &peers[i].preset.qos};
        rx[i] = (struct bt_bap_unicast_group_stream_param){.stream = &peers[i].rx.bap,
                                                           .qos = &peers[i].capture_preset.qos};
        pairs[i] =
            (struct bt_bap_unicast_group_stream_pair_param){.tx_param = &tx[i], .rx_param = &rx[i]};
    }
    struct bt_bap_unicast_group_param cfg = {
        .params = pairs, .params_count = n, .packing = BT_ISO_PACKING_SEQUENTIAL};
    int e = bt_bap_unicast_group_create(&cfg, &group);
    if (e)
        return e;
    for (unsigned i = 0; i < n; i++) {
        struct peer *p = &peers[i];
        e = bt_bap_stream_qos(p->conn, group);
        if (e)
            return e;
        e = wait_stream(p, &p->tx, ST_QOS);
        if (e)
            return e;
        e = wait_stream(p, &p->rx, ST_QOS);
        if (e)
            return e;
        e = bt_bap_stream_enable(&p->tx.bap, p->preset.codec_cfg.meta,
                                 p->preset.codec_cfg.meta_len);
        if (e)
            return e;
        e = wait_stream(p, &p->tx, ST_ENABLE);
        if (e)
            return e;
        e = bt_bap_stream_enable(&p->rx.bap, p->preset.codec_cfg.meta,
                                 p->preset.codec_cfg.meta_len);
        if (e)
            return e;
        e = wait_stream(p, &p->rx, ST_ENABLE);
        if (e)
            return e;
    }
    for (unsigned i = 0; i < n; i++) {
        struct peer *p = &peers[i];
        e = bt_bap_stream_connect(&p->tx.bap);
        if (e && e != -EALREADY)
            return e;
        e = wait_stream(p, &p->rx, ST_CONNECTED);
        if (e)
            return e;
        e = bt_bap_stream_start(&p->rx.bap);
        if (e && e != -EALREADY)
            return e;
        e = wait_stream(p, &p->tx, ST_STARTED);
        if (e)
            return e;
        e = wait_stream(p, &p->rx, ST_STARTED);
        if (e)
            return e;
    }
    /* The first CIS can receive while the second CIS is still starting. Do
     * not begin playback with a full queue of stale microphone packets. */
    for (unsigned i = 0; i < n; i++) {
        k_msgq_purge(capture_packets[i]);
        peers[i].capture_primed = false;
        peers[i].packet_pending = false;
    }
    tx_timestamp_valid = false;
    atomic_set(&streaming, 1);
    atomic_set(&ear_flags, OE_EAR_CONNECTED | OE_EAR_STREAMING | OE_NAME_VALID);
    int adv_error = proxy_advertise(n);
    printk("PROXY advertising=%d\n", adv_error);
    if (adv_error)
        return adv_error;
    printk("AUDIO started ears=%u name=%s\n", n, pair_name);
    return 0;
}
void earables_send(const int16_t *stereo) {
    k_mutex_lock(&audio_mutex, K_FOREVER);
    if (!atomic_get(&streaming)) {
        k_mutex_unlock(&audio_mutex);
        return;
    }
    struct net_buf *encoded[PEERS] = {0};
    unsigned count = 0;
    /* Encode both channels before assigning either to an ISO event. Encoding
     * the second channel after sending the first can miss that event's deadline. */
    for (unsigned i = 0; i < PEERS; i++) {
        struct peer *p = &peers[i];
        if (!(atomic_get(&p->tx.state) & ST_STARTED))
            continue;
        struct net_buf *buf = net_buf_alloc(&iso_pool, K_NO_WAIT);
        if (!buf) {
            atomic_inc(&link_errors);
            atomic_inc(&audio_tx_errors);
            continue;
        }
        net_buf_reserve(buf, BT_ISO_CHAN_SEND_RESERVE);
        int e = lc3_encode(p->encoder, LC3_PCM_FORMAT_S16, stereo + p->side, 2, OCTETS,
                           net_buf_add(buf, OCTETS));
        if (e) {
            net_buf_unref(buf);
            atomic_inc(&link_errors);
            atomic_inc(&audio_tx_errors);
        } else {
            encoded[i] = buf;
            count++;
        }
    }
    bool have_timestamp = false;
    uint32_t timestamp = 0;
    for (unsigned i = 0; i < PEERS; i++) {
        if (!encoded[i])
            continue;
        struct bt_bap_stream *stream = &peers[i].tx.bap;
        /* Nordic's time-of-arrival mode avoids dropping jittered frames due to
         * an assumed sequence-number schedule. Timestamp the second channel
         * with the first channel's controller-assigned time for stereo sync. */
        int e = have_timestamp ? bt_bap_stream_send_ts(stream, encoded[i], 0, timestamp)
                               : bt_bap_stream_send(stream, encoded[i], 0);
        if (e) {
            net_buf_unref(encoded[i]);
            atomic_inc(&audio_tx_errors);
            atomic_inc(&link_errors);
            continue;
        }
        atomic_inc(&audio_tx_packets);
        if (have_timestamp) {
            atomic_inc(&audio_tx_sync_frames);
            continue;
        }
        if (count < 2)
            continue;
        struct bt_bap_ep_info info;
        sdc_hci_cmd_vs_iso_read_tx_timestamp_t command;
        sdc_hci_cmd_vs_iso_read_tx_timestamp_return_t result;
        uint16_t handle;
        e = bt_bap_ep_get_info(stream->ep, &info);
        if (!e)
            e = info.iso_chan && info.iso_chan->iso
                    ? bt_hci_get_conn_handle(info.iso_chan->iso, &handle)
                    : -ENOTCONN;
        if (!e) {
            command.conn_handle = handle;
            e = hci_vs_sdc_iso_read_tx_timestamp(&command, &result);
        }
        if (!e) {
            timestamp = result.tx_time_stamp;
            if (tx_timestamp_valid) {
                uint32_t elapsed = timestamp - last_tx_timestamp;
                if (!elapsed)
                    atomic_inc(&audio_tx_same_event);
                else if (elapsed > 10000)
                    atomic_add(&audio_tx_skipped_events, elapsed / 10000 - 1);
            }
            last_tx_timestamp = timestamp;
            tx_timestamp_valid = true;
            have_timestamp = true;
        } else {
            atomic_inc(&audio_tx_sync_errors);
            atomic_inc(&link_errors);
        }
    }
    k_mutex_unlock(&audio_mutex);
}
void earables_capture(int16_t *stereo) {
    memset(stereo, 0, PCM_BYTES);
    k_mutex_lock(&audio_mutex, K_FOREVER);
    if (!atomic_get(&streaming)) {
        k_mutex_unlock(&audio_mutex);
        return;
    }
    int16_t mono[PCM_SAMPLES];
    unsigned seen = 0;
    for (unsigned n = 0; n < PEERS; n++) {
        struct peer *p = &peers[n];
        if (!(atomic_get(&p->rx.state) & ST_STARTED))
            continue;
        atomic_set(&audio_rx_queue_depth[n], k_msgq_num_used_get(capture_packets[n]));
        if (!p->capture_primed) {
            if (k_msgq_num_used_get(capture_packets[n]) < 6)
                continue;
            if (k_msgq_get(capture_packets[n], &p->pending, K_NO_WAIT))
                continue;
            p->capture_sequence = p->pending.seq;
            p->capture_primed = true;
            p->packet_pending = true;
        }
        while (!p->packet_pending && !k_msgq_get(capture_packets[n], &p->pending, K_NO_WAIT)) {
            /* Discard late packets; preserve a future packet while concealing
             * a missing sequence instead of playing it early. */
            if ((int16_t)(p->pending.seq - p->capture_sequence) >= 0)
                p->packet_pending = true;
            else
                atomic_inc(&audio_rx_late[n]);
        }
        const uint8_t *data = NULL;
        unsigned length = 0;
        if (p->packet_pending && (uint16_t)(p->pending.seq - p->capture_sequence) >= 8) {
            atomic_add(&audio_rx_concealed, (uint16_t)(p->pending.seq - p->capture_sequence));
            p->capture_sequence = p->pending.seq;
        }
        bool matched = p->packet_pending && p->pending.seq == p->capture_sequence;
        if (matched) {
            length = p->pending.len;
            data = length ? p->pending.data : NULL;
            p->packet_pending = false;
        }
        if (!matched)
            atomic_inc(&audio_rx_missing[n]);
        p->capture_sequence++;
        if (!length)
            atomic_inc(&audio_rx_concealed);
        lc3_decode(p->decoder, data, length, LC3_PCM_FORMAT_S16, mono, 1);
        for (unsigned j = 0; j < PCM_SAMPLES; j++)
            stereo[j * 2 + p->side] = mono[j];
        seen |= 1u << p->side;
    }
    /* Single-ear operation keeps full gain; the ESP averages both channels. */
    if (seen == 1 || seen == 2)
        for (unsigned j = 0; j < PCM_SAMPLES; j++)
            stereo[j * 2 + (seen == 1 ? 1 : 0)] = stereo[j * 2 + (seen == 1 ? 0 : 1)];
    k_mutex_unlock(&audio_mutex);
}
static void cleanup(void) {
    atomic_set(&skip_saved_first, 0);
    atomic_set(&streaming, 0);
    atomic_and(&ear_flags, OE_NAME_VALID);
    k_mutex_lock(&audio_mutex, K_FOREVER);
    k_mutex_unlock(&audio_mutex);
    for (unsigned i = 0; i < PEERS; i++)
        if (peers[i].conn)
            bt_conn_disconnect(peers[i].conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    /* Keep stream and subscription storage alive until the host has finished
     * every disconnect callback. Never reuse memory still owned by a CIG. */
    for (unsigned i = 0; i < PEERS; i++)
        if (peers[i].conn) {
            struct bt_conn_info info;
            while (!bt_conn_get_info(peers[i].conn, &info) &&
                   info.state != BT_CONN_STATE_DISCONNECTED)
                k_sleep(K_MSEC(20));
        }
    proxy_reset();
    while (group) {
        int e = bt_bap_unicast_group_delete(group);
        if (!e)
            group = NULL;
        else {
            printk("AUDIO waiting for group release=%d\n", e);
            k_sleep(K_MSEC(200));
        }
    }
    for (unsigned i = 0; i < PEERS; i++) {
        if (peers[i].conn)
            bt_conn_unref(peers[i].conn);
        memset(&peers[i], 0, sizeof(peers[i]));
        peers[i].tx.bap.ops = &ops;
        peers[i].rx.bap.ops = &ops;
    }
    for (unsigned i = 0; i < PEERS; i++)
        k_msgq_purge(capture_packets[i]);
    k_msgq_purge(&candidates);
}
static void discard_pending(unsigned index) {
    struct peer *p = &peers[index];
    __ASSERT_NO_MSG(!group && !atomic_get(&streaming));
    if (p->conn) {
        bt_conn_disconnect(p->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        struct bt_conn_info info;
        while (!bt_conn_get_info(p->conn, &info) && info.state != BT_CONN_STATE_DISCONNECTED)
            k_sleep(K_MSEC(20));
    }
    proxy_discard_pending(index);
    if (p->conn)
        bt_conn_unref(p->conn);
    memset(p, 0, sizeof(*p));
    p->tx.bap.ops = &ops;
    p->rx.bap.ops = &ops;
    k_msgq_purge(capture_packets[index]);
}
static int next_candidate(struct candidate *candidate, int seconds, const bt_addr_le_t *failed,
                          unsigned failed_count) {
    int64_t deadline = k_uptime_get() + seconds * 1000;
    while (k_uptime_get() < deadline) {
        if (k_sem_count_get(&pair_request))
            return -ECANCELED;
        if (!k_msgq_get(&candidates, candidate, K_MSEC(100))) {
            bool excluded = false;
            for (unsigned i = 0; i < failed_count; i++)
                excluded |= bt_addr_le_eq(&candidate->addr, &failed[i]);
            if (!excluded)
                return 0;
        }
    }
    return -ETIMEDOUT;
}
static void run(void *a, void *b, void *c) {
    cleanup();
    for (;;) {
        k_sem_take(&pair_request, K_FOREVER);
        cleanup();
        int mode = atomic_get(&request_mode);
        if (mode == 4) {
            printk("EAR suspended; saved pair retained\n");
            continue;
        }
        if (mode == 3) {
            saved.count = 0;
            saved.name[0] = 0;
            saved.hardware_revision[0] = 0;
            saved.reset_epoch++;
            for (unsigned id = 0; id < CONFIG_BT_ID_MAX; id++)
                bt_unpair(id, NULL);
            settings_save_one("adapter/pair", &saved, sizeof(saved));
            k_mutex_lock(&name_mutex, K_FOREVER);
            strcpy(pair_name, "OpenEarable Adapter");
            pair_hardware_revision[0] = 0;
            atomic_set(&ear_flags, 0);
            atomic_set(&reset_epoch, saved.reset_epoch);
            k_mutex_unlock(&name_mutex);
            bt_set_name("OpenEarable Adapter");
            printk("RESET complete\n");
            continue;
        }
        atomic_set(&pairing, mode == 1);
        if (mode == 1)
            atomic_or(&ear_flags, OE_EAR_PAIRING);
        atomic_set(&skip_saved_first, mode == 1);
        unsigned count = 0, saved_index = 0, failed_count = 0;
        bt_addr_le_t failed[8];
        int e = 0;
        int64_t deadline = k_uptime_get() + 60000;
        while (count < PEERS && (mode != 2 || saved_index < saved.count) &&
               k_uptime_get() < deadline) {
            struct candidate candidate = {0};
            if (mode == 2) {
                candidate.addr = saved.address[saved_index++];
                memcpy(candidate.name, saved.name, sizeof(candidate.name));
            } else {
                e = bt_le_scan_start(BT_LE_SCAN_ACTIVE, NULL);
                if (e && e != -EALREADY)
                    break;
                e = next_candidate(&candidate, count ? 15 : 40, failed, failed_count);
                bt_le_scan_stop();
                if (e)
                    break;
            }
            char address[BT_ADDR_LE_STR_LEN];
            bt_addr_le_to_str(&candidate.addr, address, sizeof(address));
            printk("EAR connecting %s address=%s\n", candidate.name, address);
            e = connect_peer(&peers[count], &candidate);
            if (!e && count && peers[count].side == peers[0].side) {
                e = -EINVAL;
                printk("EAR pair mismatch\n");
            }
            if (e) {
                printk("EAR setup error=%d\n", e);
                discard_pending(count);
                if (e == -ECANCELED)
                    break;
                if (failed_count == ARRAY_SIZE(failed))
                    break;
                failed[failed_count++] = candidate.addr;
                continue;
            }
            count++;
            atomic_set(&skip_saved_first, 0);
            k_msgq_purge(&candidates);
        }
        atomic_set(&pairing, 0);
        atomic_and(&ear_flags, ~OE_EAR_PAIRING);
        if (count && e != -ECANCELED && !k_sem_count_get(&pair_request)) {
            k_mutex_lock(&name_mutex, K_FOREVER);
            unsigned primary = count == 2 && peers[1].side == 0 ? 1 : 0;
            memcpy(pair_name, peers[primary].name, sizeof(pair_name));
            memcpy(pair_hardware_revision, peers[primary].hardware_revision,
                   sizeof(pair_hardware_revision));
            k_mutex_unlock(&name_mutex);
            bt_set_name(pair_name);
            e = start(count);
            if (e)
                printk("AUDIO start error=%d\n", e);
        } else if (count)
            e = -ECANCELED;
        if (!count || e) {
            cleanup();
            if (saved.count && k_sem_count_get(&pair_request) == 0) {
                k_sleep(K_SECONDS(5));
                request_pairing(2, true);
            }
            continue;
        }
        /* A missing member must not be replaced by the surviving member's
         * address. Preserve the complete saved pair across partial reconnects. */
        if (mode != 2) {
            saved.count = count;
            for (unsigned i = 0; i < count; i++)
                bt_addr_le_copy(&saved.address[i], bt_conn_get_dst(peers[i].conn));
        }
        memcpy(saved.name, pair_name, sizeof(saved.name));
        memcpy(saved.hardware_revision, pair_hardware_revision, sizeof(saved.hardware_revision));
        settings_save_one("adapter/pair", &saved, sizeof(saved));
        while (atomic_get(&streaming) && k_sem_count_get(&pair_request) == 0)
            k_sleep(K_MSEC(100));
        cleanup();
        request_pairing(2, true);
    }
}
static K_THREAD_STACK_DEFINE(stack, 8192);
static struct k_thread thread;
void earables_reset(void) { request_pairing(3, false); }
void earables_suspend(void) { request_pairing(4, false); }
void earables_resume(void) { request_pairing(2, false); }
void earables_radio_status(void) {
    for (unsigned i = 0; i < PEERS; i++) {
        if (!peers[i].connected)
            continue;
        uint16_t handle;
        if (bt_hci_get_conn_handle(peers[i].conn, &handle))
            continue;
        struct net_buf *cmd = bt_hci_cmd_create(BT_HCI_OP_READ_RSSI, sizeof(uint16_t));
        if (!cmd)
            continue;
        net_buf_add_le16(cmd, handle);
        struct net_buf *rsp = NULL;
        int e = bt_hci_cmd_send_sync(BT_HCI_OP_READ_RSSI, cmd, &rsp);
        if (!e) {
            struct bt_hci_rp_read_rssi *rssi = (void *)rsp->data;
            struct bt_conn_info info;
            unsigned interval = bt_conn_get_info(peers[i].conn, &info) ? 0 : info.le.interval;
            printk("RADIO ear=%u side=%u rssi=%d interval=%u packets=%ld invalid=%ld\n", i,
                   peers[i].side, rssi->rssi, interval, (long)atomic_get(&audio_peer_rx[i]),
                   (long)atomic_get(&audio_peer_invalid[i]));
#if defined(CONFIG_BT_USER_PHY_UPDATE) && defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
            if (interval)
                printk("ACL ear=%u phy=%u/%u length=%u/%u time=%u/%u\n", i,
                       info.le.phy->tx_phy, info.le.phy->rx_phy,
                       info.le.data_len->tx_max_len, info.le.data_len->rx_max_len,
                       info.le.data_len->tx_max_time, info.le.data_len->rx_max_time);
#endif
        }
        if (rsp)
            net_buf_unref(rsp);
        struct bt_bap_ep_info ep;
        if (bt_bap_ep_get_info(peers[i].tx.bap.ep, &ep) || !ep.iso_chan ||
            !ep.iso_chan->iso || bt_hci_get_conn_handle(ep.iso_chan->iso, &handle))
            continue;
        struct bt_iso_info iso;
        if (!bt_iso_chan_get_info(ep.iso_chan, &iso))
            printk("ISO_TIMING ear=%u cig=%u cis=%u subinterval=%u tx_latency=%u rx_latency=%u\n",
                   i, iso.unicast.cig_sync_delay, iso.unicast.cis_sync_delay,
                   iso.unicast.subinterval, iso.unicast.central.latency,
                   iso.unicast.peripheral.latency);
        cmd = bt_hci_cmd_create(BT_HCI_OP_LE_READ_ISO_LINK_QUALITY, sizeof(uint16_t));
        if (!cmd)
            continue;
        net_buf_add_le16(cmd, handle);
        rsp = NULL;
        e = bt_hci_cmd_send_sync(BT_HCI_OP_LE_READ_ISO_LINK_QUALITY, cmd, &rsp);
        if (!e && rsp && rsp->len >= sizeof(struct bt_hci_rp_le_read_iso_link_quality)) {
            const struct bt_hci_rp_le_read_iso_link_quality *q = (void *)rsp->data;
            printk("ISO_QUALITY ear=%u status=%u unacked=%lu flushed=%lu retransmitted=%lu crc=%lu unreceived=%lu duplicate=%lu\n",
                   i, q->status, (unsigned long)sys_le32_to_cpu(q->tx_unacked_packets),
                   (unsigned long)sys_le32_to_cpu(q->tx_flushed_packets),
                   (unsigned long)sys_le32_to_cpu(q->retransmitted_packets),
                   (unsigned long)sys_le32_to_cpu(q->crc_error_packets),
                   (unsigned long)sys_le32_to_cpu(q->rx_unreceived_packets),
                   (unsigned long)sys_le32_to_cpu(q->duplicate_packets));
        } else {
            printk("ISO_QUALITY ear=%u error=%d\n", i, e);
        }
        if (rsp)
            net_buf_unref(rsp);
    }
}
void earables_pair(void) {
    request_pairing(1, false);
    printk("PAIR requested\n");
}
void earables_init(void) {
    int e = bt_enable(NULL);
    printk("BT init=%d\n", e);
    if (e)
        return;
    /* ISO builds default to a zero-length ACL reservation, which restricts
     * data-length negotiation and starves mirrored sensor notifications.
     * Reserve 2.5 ms every 10 ms for ACL scheduling before any connection. */
    sdc_hci_cmd_vs_event_length_set_t acl = {.event_length_us = 2500};
    sdc_hci_cmd_vs_central_acl_event_spacing_set_t spacing = {
        .central_acl_event_spacing_us = 10000};
    e = hci_vs_sdc_event_length_set(&acl);
    if (!e)
        e = hci_vs_sdc_central_acl_event_spacing_set(&spacing);
    printk("RADIO ACL scheduling=%d\n", e);
    if (e)
        return;
    settings_load();
    proxy_init();
    e = bt_bap_unicast_client_register_cb(&client);
    printk("BAP init=%d\n", e);
    bt_csip_set_coordinator_register_cb(&set_callbacks);
    bt_le_scan_cb_register(&scan_callbacks);
    k_thread_create(&thread, stack, K_THREAD_STACK_SIZEOF(stack), run, NULL, NULL, NULL, 7, 0,
                    K_NO_WAIT);
    if (saved.count)
        request_pairing(2, true);
}
