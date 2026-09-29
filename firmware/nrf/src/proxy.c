/* SPDX-License-Identifier: Apache-2.0 */
#include "adapter.h"
#include "proxy.h"
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/byteorder.h>
#define MAX_ATTRS 480
#define MAX_SERVICES 40
#define MAX_CHARS 160
#define MAX_VALUE 512
union uuid_store {
    struct bt_uuid uuid;
    struct bt_uuid_16 u16;
    struct bt_uuid_128 u128;
};
struct mapping {
    union uuid_store uuid;
    uint16_t remote[2];
    int16_t value_index;
    uint8_t properties;
    bool forward;
};
static struct bt_gatt_attr attrs[MAX_ATTRS];
static struct mapping maps[MAX_ATTRS];
static struct bt_gatt_chrc declarations[MAX_CHARS];
static struct bt_gatt_service services[MAX_SERVICES];
/* BT_UUID_GATT_PRIMARY expands to a compound literal. A database that outlives
 * its builder needs a UUID with static storage duration. */
static const struct bt_uuid_16 primary_uuid = BT_UUID_INIT_16(BT_UUID_GATT_PRIMARY_VAL);
static unsigned attr_count, service_count, char_count;
static struct bt_conn *ears[2], *hosts[2];
static struct bt_le_ext_adv *advertisers[2];
static uint8_t identities[2];
static char ear_names[2][64];
static unsigned expected_peers;
static K_MUTEX_DEFINE(io_mutex);
static K_MUTEX_DEFINE(request_mutex);
static K_MUTEX_DEFINE(notify_mutex_0);
static K_MUTEX_DEFINE(notify_mutex_1);
static struct k_mutex *const notify_mutexes[] = {&notify_mutex_0, &notify_mutex_1};
static atomic_t active, generation;
static atomic_t host_generation[2], host_cleanup_pending;
static void resume_advertising(void);
static struct k_spinlock host_lock;
static K_SEM_DEFINE(io_done, 0, 1);
static volatile int io_error;
static uint8_t *io_data;
static size_t io_size, io_used;
/* Keep remote ATT error codes separate from local errno values. */
static int remote_error(uint8_t error) { return error ? -(256 + error) : 0; }
static uint8_t att_error(int error) {
    if (error <= -257 && error >= -511)
        return -error - 256;
    return error == -ENOMEM ? BT_ATT_ERR_INSUFFICIENT_RESOURCES : BT_ATT_ERR_UNLIKELY;
}
static void uuid_copy(union uuid_store *dst, const struct bt_uuid *src) {
    memcpy(dst, src,
           src->type == BT_UUID_TYPE_16 ? sizeof(struct bt_uuid_16) : sizeof(struct bt_uuid_128));
}
static uint8_t read_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_read_params *p,
                       const void *data, uint16_t len) {
    io_error = err;
    if (err || !data) {
        k_sem_give(&io_done);
        return BT_GATT_ITER_STOP;
    }
    size_t n = MIN(len, io_size - io_used);
    memcpy(io_data + io_used, data, n);
    io_used += n;
    if (io_used == io_size || p->handle_count == 0) {
        k_sem_give(&io_done);
        return BT_GATT_ITER_STOP;
    }
    return BT_GATT_ITER_CONTINUE;
}
static int read_value(struct bt_conn *conn, uint16_t handle, uint16_t offset,
                      const struct bt_uuid *uuid, void *data, size_t size) {
    k_mutex_lock(&io_mutex, K_FOREVER);
    k_sem_reset(&io_done);
    io_error = 0;
    io_data = data;
    io_size = size;
    io_used = 0;
    struct bt_gatt_read_params p = {.func = read_cb, .handle_count = uuid ? 0 : 1};
    if (uuid) {
        p.by_uuid.start_handle = 1;
        p.by_uuid.end_handle = 0xffff;
        p.by_uuid.uuid = uuid;
    } else {
        p.single.handle = handle;
        p.single.offset = offset;
    }
    int e = bt_gatt_read(conn, &p);
    if (!e && k_sem_take(&io_done, K_SECONDS(5))) {
        bt_gatt_cancel(conn, &p);
        e = -ETIMEDOUT;
    }
    if (!e)
        e = io_error ? remote_error(io_error) : (int)io_used;
    k_mutex_unlock(&io_mutex);
    return e;
}
int proxy_read_uuid(struct bt_conn *conn, const struct bt_uuid *uuid, void *data, size_t size) {
    return read_value(conn, 0, 0, uuid, data, size);
}
static void write_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_write_params *p) {
    io_error = err;
    k_sem_give(&io_done);
}
static int write_value(struct bt_conn *conn, uint16_t handle, const void *data, uint16_t len) {
    k_mutex_lock(&io_mutex, K_FOREVER);
    k_sem_reset(&io_done);
    io_error = 0;
    struct bt_gatt_write_params p = {
        .func = write_cb, .handle = handle, .data = data, .length = len};
    int e = bt_gatt_write(conn, &p);
    if (!e && k_sem_take(&io_done, K_SECONDS(5))) {
        bt_gatt_cancel(conn, &p);
        e = -ETIMEDOUT;
    }
    if (!e)
        e = remote_error(io_error);
    k_mutex_unlock(&io_mutex);
    return e;
}
int proxy_set_render_volume(unsigned peer, struct bt_conn *conn, uint8_t volume) {
    uint16_t state_handle = 0, control_handle = 0;
    for (unsigned i = 0; i < attr_count; i++) {
        if (maps[i].uuid.uuid.type != BT_UUID_TYPE_16)
            continue;
        uint16_t uuid = maps[i].uuid.u16.val;
        if (uuid == 0x2b7d)
            state_handle = maps[i].remote[peer];
        else if (uuid == 0x2b7e)
            control_handle = maps[i].remote[peer];
    }
    if (!state_handle || !control_handle)
        return -ENOTSUP;
    uint8_t state[3];
    int e = read_value(conn, state_handle, 0, NULL, state, sizeof(state));
    if (e != sizeof(state))
        return e < 0 ? e : -EPROTO;
    uint8_t set[] = {0x04, state[2], volume}; // VCP Set Absolute Volume, change counter.
    e = write_value(conn, control_handle, set, sizeof(set));
    if (e)
        return e;
    e = read_value(conn, state_handle, 0, NULL, state, sizeof(state));
    if (e != sizeof(state) || state[0] != volume)
        return e < 0 ? e : -EPROTO;
    if (state[1]) {
        uint8_t unmute[] = {0x06, state[2]};
        e = write_value(conn, control_handle, unmute, sizeof(unmute));
        if (e)
            return e;
    }
    printk("VOLUME ear=%u value=%u\n", peer, volume);
    return 0;
}
struct remote_service {
    union uuid_store uuid;
    uint16_t start, end;
};
struct remote_char {
    union uuid_store uuid;
    uint16_t decl, value;
    uint8_t properties;
};
static struct remote_service remote_services[MAX_SERVICES];
static unsigned remote_service_count;
static struct remote_char remote_chars[MAX_CHARS];
static unsigned remote_char_count;
struct descriptor {
    union uuid_store uuid;
    uint16_t handle;
};
static struct descriptor descriptors[32];
static unsigned descriptor_count;
static uint8_t discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *a,
                           struct bt_gatt_discover_params *p) {
    if (!a) {
        k_sem_give(&io_done);
        return BT_GATT_ITER_STOP;
    }
    if (p->type == BT_GATT_DISCOVER_PRIMARY) {
        if (remote_service_count == MAX_SERVICES) {
            io_error = -ENOSPC;
            goto stop;
        }
        struct bt_gatt_service_val *v = a->user_data;
        struct remote_service *s = &remote_services[remote_service_count++];
        uuid_copy(&s->uuid, v->uuid);
        s->start = a->handle;
        s->end = v->end_handle;
    } else if (p->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
        if (remote_char_count == MAX_CHARS) {
            io_error = -ENOSPC;
            goto stop;
        }
        struct bt_gatt_chrc *v = a->user_data;
        struct remote_char *c = &remote_chars[remote_char_count++];
        uuid_copy(&c->uuid, v->uuid);
        c->decl = a->handle;
        c->value = v->value_handle;
        c->properties = v->properties;
    } else {
        if (descriptor_count == ARRAY_SIZE(descriptors)) {
            io_error = -ENOSPC;
            goto stop;
        }
        struct descriptor *d = &descriptors[descriptor_count++];
        uuid_copy(&d->uuid, a->uuid);
        d->handle = a->handle;
    }
    return BT_GATT_ITER_CONTINUE;
stop:
    k_sem_give(&io_done);
    return BT_GATT_ITER_STOP;
}
static int discover(struct bt_conn *conn, uint8_t type, uint16_t start, uint16_t end) {
    k_sem_reset(&io_done);
    io_error = 0;
    struct bt_gatt_discover_params p = {
        .func = discover_cb, .type = type, .start_handle = start, .end_handle = end};
    int e = bt_gatt_discover(conn, &p);
    if (!e && k_sem_take(&io_done, K_SECONDS(10))) {
        bt_gatt_cancel(conn, &p);
        e = -ETIMEDOUT;
    }
    return e ? e : io_error;
}
static bool skip_service(const struct bt_uuid *uuid) {
    if (uuid->type != BT_UUID_TYPE_16)
        return false;
    uint16_t v = BT_UUID_16(uuid)->val;
    // Transport and set coordination terminate at the adapter; app services are mirrored.
    return v == 0x1800 || v == 0x1801 || v == 0x184e || v == 0x1850 || v == 0x1846 || v == 0x1853;
}
static ssize_t unavailable(struct bt_conn *c, const struct bt_gatt_attr *a, void *b, uint16_t l,
                           uint16_t o) {
    return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
}
static int add_attr(unsigned peer, unsigned index, const struct bt_uuid *uuid, uint16_t remote,
                    bool forward, uint8_t properties, int16_t value_index) {
    if (index >= MAX_ATTRS)
        return -ENOSPC;
    struct mapping *m = &maps[index];
    if (peer) {
        if (index >= attr_count || bt_uuid_cmp(&m->uuid.uuid, uuid))
            return -EPROTO;
    } else {
        uuid_copy(&m->uuid, uuid);
        m->forward = forward;
        m->properties = properties;
        m->value_index = value_index;
        attrs[index] = (struct bt_gatt_attr){.uuid = &m->uuid.uuid,
                                             .perm = BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                                             .read = unavailable};
        attr_count = index + 1;
    }
    m->remote[peer] = remote;
    return 0;
}
int proxy_attach(unsigned peer, struct bt_conn *conn, char *name, size_t size) {
    int e = proxy_read_uuid(conn, BT_UUID_GAP_DEVICE_NAME, name, size - 1);
    if (e < 0)
        return e;
    name[e] = 0;
    if (strncmp(name, "OpenEarable", 11) != 0)
        return -EPERM;
    strncpy(ear_names[peer], name, 63);
    k_mutex_lock(&io_mutex, K_FOREVER);
    remote_service_count = 0;
    e = discover(conn, BT_GATT_DISCOVER_PRIMARY, 1, 0xffff);
    if (e)
        goto out;
    unsigned index = 0, service_index = 0, decl_index = 0;
    for (unsigned s = 0; s < remote_service_count; s++) {
        struct remote_service *rs = &remote_services[s];
        if (skip_service(&rs->uuid.uuid))
            continue;
        unsigned begin = index;
        e = add_attr(peer, index, &rs->uuid.uuid, rs->start, false, 0, -1);
        if (e)
            goto out;
        if (!peer) {
            attrs[index].uuid = &primary_uuid.uuid;
            attrs[index].user_data = &maps[index].uuid;
            attrs[index].read = bt_gatt_attr_read_service;
            attrs[index].perm = BT_GATT_PERM_READ;
        }
        index++;
        remote_char_count = 0;
        e = discover(conn, BT_GATT_DISCOVER_CHARACTERISTIC, rs->start + 1, rs->end);
        if (e)
            goto out;
        for (unsigned c = 0; c < remote_char_count; c++) {
            struct remote_char *rc = &remote_chars[c];
            if (decl_index >= MAX_CHARS) {
                e = -ENOSPC;
                goto out;
            }
            e = add_attr(peer, index, BT_UUID_GATT_CHRC, rc->decl, false, rc->properties,
                         index + 1);
            if (e)
                goto out;
            if (!peer) {
                declarations[decl_index] = (struct bt_gatt_chrc){.uuid = &maps[index + 1].uuid.uuid,
                                                                 .properties = rc->properties};
                attrs[index].user_data = &declarations[decl_index];
                attrs[index].read = bt_gatt_attr_read_chrc;
                attrs[index].perm = BT_GATT_PERM_READ;
            }
            index++;
            decl_index++;
            unsigned value = index;
            e = add_attr(peer, index++, &rc->uuid.uuid, rc->value, true, rc->properties, -1);
            if (e)
                goto out;
            uint16_t end = c + 1 < remote_char_count ? remote_chars[c + 1].decl - 1 : rs->end;
            descriptor_count = 0;
            if (rc->value < end) {
                e = discover(conn, BT_GATT_DISCOVER_DESCRIPTOR, rc->value + 1, end);
                if (e)
                    goto out;
            }
            for (unsigned d = 0; d < descriptor_count; d++) {
                e = add_attr(peer, index++, &descriptors[d].uuid.uuid, descriptors[d].handle, true,
                             0, value);
                if (e)
                    goto out;
            }
        }
        if (!peer) {
            services[service_index].attrs = &attrs[begin];
            services[service_index].attr_count = index - begin;
        }
        service_index++;
    }
    if (peer && index != attr_count) {
        e = -EPROTO;
        goto out;
    }
    if (!peer) {
        service_count = service_index;
        char_count = decl_index;
        for (unsigned s = 0; s < service_count; s++) {
            e = bt_gatt_service_register(&services[s]);
            if (e)
                goto out;
            printk("PROXY service first=%u count=%u\n", services[s].attrs[0].handle,
                   services[s].attr_count);
        }
    }
    ears[peer] = bt_conn_ref(conn);
    printk("PROXY ear=%u services=%u attributes=%u name=%s\n", peer, service_index, index, name);
out:
    k_mutex_unlock(&io_mutex);
    return e;
}
/* Only used during discovery, before any proxy has been advertised. Keep the
 * first ear's shared schema while discarding an unsuccessful second member. */
void proxy_discard_pending(unsigned peer) {
    __ASSERT_NO_MSG(peer < 2 && !atomic_get(&active));
    if (!peer) {
        proxy_reset();
        return;
    }
    k_mutex_lock(&io_mutex, K_FOREVER);
    if (ears[peer]) {
        bt_conn_unref(ears[peer]);
        ears[peer] = NULL;
    }
    for (unsigned i = 0; i < attr_count; i++)
        maps[i].remote[peer] = 0;
    ear_names[peer][0] = 0;
    k_mutex_unlock(&io_mutex);
}
extern int adapter_att_reply(struct bt_conn *, uint8_t, const void *, size_t);
struct request {
    struct bt_conn *host;
    uint32_t generation, session;
    uint16_t index, length, offset;
    uint8_t peer, op;
    uint8_t data[244];
};
K_MSGQ_DEFINE(requests, sizeof(struct request), 20, 4);
struct notification {
    uint32_t generation, session;
    uint16_t index, length;
    uint8_t peer, kind;
    uint8_t data[244];
};
K_MSGQ_DEFINE(notifications_0, sizeof(struct notification), 32, 4);
K_MSGQ_DEFINE(notifications_1, sizeof(struct notification), 32, 4);
static struct k_msgq *const notification_queues[] = {&notifications_0, &notifications_1};
atomic_t proxy_notify_received[2], proxy_notify_sent[2], proxy_notify_dropped[2];
atomic_t proxy_notify_errors[2], proxy_notify_oversize[2];
atomic_t proxy_notify_last_error[2], proxy_notify_queue_peak[2];
struct subscription {
    struct bt_gatt_subscribe_params params;
    uint16_t index, ccc_index;
    uint8_t peer;
};
static struct subscription subscriptions[2][MAX_CHARS];
static unsigned subscription_count[2];
static atomic_t subscribed_values[2][MAX_ATTRS];
static uint8_t notified(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                        const void *data, uint16_t length) {
    struct subscription *s = CONTAINER_OF(params, struct subscription, params);
    if (!data)
        return BT_GATT_ITER_STOP;
    if (!atomic_get(&active) || !atomic_get(&subscribed_values[s->peer][s->index]))
        return BT_GATT_ITER_CONTINUE;
    atomic_inc(&proxy_notify_received[s->peer]);
    struct notification n = {.generation = atomic_get(&generation),
                             .session = atomic_get(&host_generation[s->peer]),
                             .index = s->index,
                             .peer = s->peer,
                             .length = length,
                             .kind = params->value};
    if (length > sizeof(n.data)) {
        atomic_inc(&proxy_notify_oversize[s->peer]);
        atomic_inc(&link_errors);
        return BT_GATT_ITER_CONTINUE;
    }
    memcpy(n.data, data, length);
    if (k_msgq_put(notification_queues[s->peer], &n, K_NO_WAIT)) {
        atomic_inc(&proxy_notify_dropped[s->peer]);
        atomic_inc(&link_errors);
    }
    unsigned queued = k_msgq_num_used_get(notification_queues[s->peer]);
    if (queued > atomic_get(&proxy_notify_queue_peak[s->peer]))
        atomic_set(&proxy_notify_queue_peak[s->peer], queued);
    return BT_GATT_ITER_CONTINUE;
}
static void subscribed(struct bt_conn *conn, uint8_t err, struct bt_gatt_subscribe_params *p) {
    io_error = err;
    k_sem_give(&io_done);
}
static int subscribe_value(unsigned peer, unsigned index, const void *data, unsigned len) {
    if (len != 2)
        return remote_error(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    uint16_t value = sys_get_le16(data);
    struct mapping *m = &maps[index];
    if (m->value_index < 0 || value > 3)
        return remote_error(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    unsigned v = m->value_index;
    struct subscription *s = NULL;
    for (unsigned i = 0; i < subscription_count[peer]; i++)
        if (subscriptions[peer][i].index == v) {
            s = &subscriptions[peer][i];
            break;
        }
    if (s && s->params.value == value)
        return 0;
    if (!s) {
        if (subscription_count[peer] == MAX_CHARS)
            return remote_error(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
        s = &subscriptions[peer][subscription_count[peer]++];
        s->index = v;
        s->ccc_index = index;
        s->peer = peer;
    } else if (s->params.value) {
        /* The CCC response still refers to this object. Wait for it before
         * reusing the same subscription storage for another value. */
        k_mutex_lock(&io_mutex, K_FOREVER);
        k_sem_reset(&io_done);
        io_error = 0;
        int e = bt_gatt_unsubscribe(ears[peer], &s->params);
        if (!e && k_sem_take(&io_done, K_SECONDS(5))) {
            bt_gatt_cancel(ears[peer], &s->params);
            e = -ETIMEDOUT;
        }
        k_mutex_unlock(&io_mutex);
        if (e)
            return e;
    }
    atomic_set(&subscribed_values[peer][v], 0);
    if (!value)
        return 0;
    s->params = (struct bt_gatt_subscribe_params){.notify = notified,
                                                  .subscribe = subscribed,
                                                  .value_handle = maps[v].remote[peer],
                                                  .ccc_handle = m->remote[peer],
                                                  .value = value};
    atomic_set_bit(s->params.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);
    k_mutex_lock(&io_mutex, K_FOREVER);
    k_sem_reset(&io_done);
    io_error = 0;
    int e = bt_gatt_subscribe(ears[peer], &s->params);
    if (!e && k_sem_take(&io_done, K_SECONDS(5))) {
        bt_gatt_cancel(ears[peer], &s->params);
        e = -ETIMEDOUT;
    }
    if (!e)
        e = remote_error(io_error);
    if (!e)
        atomic_set(&subscribed_values[peer][v], value);
    k_mutex_unlock(&io_mutex);
    return e;
}
bool adapter_att_request(struct bt_conn *conn, const uint8_t *data, size_t len) {
    if (!atomic_get(&active))
        return false;
    if (len < 3)
        return false;
    int peer = conn == hosts[0] ? 0 : conn == hosts[1] ? 1 : -1;
    if (peer < 0)
        return false;
    uint8_t op = data[0];
    if (op != 0x0a && op != 0x0c && op != 0x12 && op != 0x52)
        return false;
    uint16_t handle = sys_get_le16(data + 1);
    unsigned index;
    for (index = 0; index < attr_count; index++)
        if (attrs[index].handle == handle && maps[index].forward)
            break;
    if (index == attr_count)
        return false;
    struct request r = {.host = bt_conn_ref(conn),
                        .generation = atomic_get(&generation),
                        .session = atomic_get(&host_generation[peer]),
                        .peer = peer,
                        .index = index,
                        .op = op};
    if (op == 0x0c) {
        if (len != 5) {
            bt_conn_unref(r.host);
            return false;
        }
        r.offset = sys_get_le16(data + 3);
    }
    if (op == 0x12 || op == 0x52) {
        r.length = len - 3;
        if (r.length > sizeof(r.data)) {
            bt_conn_unref(r.host);
            return false;
        }
        memcpy(r.data, data + 3, r.length);
    }
    if (k_msgq_put(&requests, &r, K_NO_WAIT)) {
        bt_conn_unref(r.host);
        if (op != 0x52) {
            uint8_t err[] = {op, handle & 255, handle >> 8, BT_ATT_ERR_INSUFFICIENT_RESOURCES};
            adapter_att_reply(conn, 0x01, err, sizeof(err));
        }
    }
    return true;
}
static void request_thread(void *a, void *b, void *c) {
    struct request r;
    uint8_t value[MAX_VALUE];
    for (;;) {
        k_msgq_get(&requests, &r, K_FOREVER);
        int e = -BT_ATT_ERR_UNLIKELY;
        unsigned size = 0;
        k_mutex_lock(&request_mutex, K_FOREVER);
        for (unsigned peer = 0; peer < 2; peer++)
            if (atomic_test_bit(&host_cleanup_pending, peer)) {
                if (atomic_get(&active) && ears[peer]) {
                    const uint8_t off[2] = {0};
                    for (unsigned i = 0; i < subscription_count[peer]; i++)
                        if (subscriptions[peer][i].params.value) {
                            int err = subscribe_value(peer, subscriptions[peer][i].ccc_index, off,
                                                      sizeof(off));
                            if (err)
                                printk("PROXY unsubscribe ear=%u error=%d\n", peer, err);
                        }
                }
                atomic_clear_bit(&host_cleanup_pending, peer);
                if (atomic_get(&active))
                    resume_advertising();
            }
        if (!r.host) {
            k_mutex_unlock(&request_mutex);
            continue;
        }
        if (!atomic_get(&active) || r.generation != atomic_get(&generation) ||
            r.session != atomic_get(&host_generation[r.peer])) {
            bt_conn_unref(r.host);
            k_mutex_unlock(&request_mutex);
            continue;
        }
        struct bt_conn *ear = ears[r.peer] ? bt_conn_ref(ears[r.peer]) : NULL;
        if (ear) {
            uint16_t handle = maps[r.index].remote[r.peer];
            if (r.op == 0x0a || r.op == 0x0c) {
                if (!bt_uuid_cmp(attrs[r.index].uuid, BT_UUID_GATT_CCC)) {
                    if (r.offset > 2)
                        e = remote_error(BT_ATT_ERR_INVALID_OFFSET);
                    else {
                        sys_put_le16(
                            atomic_get(&subscribed_values[r.peer][maps[r.index].value_index]),
                            value);
                        size = 2 - r.offset;
                        memmove(value, value + r.offset, size);
                        e = 0;
                    }
                } else {
                    e = read_value(ear, handle, r.offset, NULL, value,
                                   MIN(sizeof(value), bt_gatt_get_mtu(r.host) - 1));
                    if (e >= 0) {
                        size = e;
                        e = 0;
                    }
                }
            } else if (!bt_uuid_cmp(attrs[r.index].uuid, BT_UUID_GATT_CCC))
                e = subscribe_value(r.peer, r.index, r.data, r.length);
            else if (r.op == 0x52)
                e = bt_gatt_write_without_response(ear, handle, r.data, r.length, false);
            else
                e = write_value(ear, handle, r.data, r.length);
            bt_conn_unref(ear);
        }
        if (r.op != 0x52) {
            if (e) {
                uint16_t h = attrs[r.index].handle;
                uint8_t error[] = {r.op, h & 255, h >> 8, att_error(e)};
                adapter_att_reply(r.host, 1, error, sizeof(error));
            } else
                adapter_att_reply(r.host,
                                  r.op == 0x12   ? 0x13
                                  : r.op == 0x0c ? 0x0d
                                                 : 0x0b,
                                  value, size);
        }
        bt_conn_unref(r.host);
        k_mutex_unlock(&request_mutex);
    }
}
static K_SEM_DEFINE(indication_done, 0, 1);
static K_MUTEX_DEFINE(indication_mutex);
/* Four in flight per host allow several packets in a connection event while
 * leaving four of the twelve ACL buffers for control traffic. */
static K_SEM_DEFINE(notification_slots_0, 4, 4);
static K_SEM_DEFINE(notification_slots_1, 4, 4);
static struct k_sem *const notification_slots[] = {&notification_slots_0, &notification_slots_1};
static void notification_sent(struct bt_conn *conn, void *user_data) {
    uint32_t session = (uintptr_t)user_data;
    unsigned peer = session & 1;
    if ((session >> 1) == (uint32_t)atomic_get(&host_generation[peer])) {
        atomic_inc(&proxy_notify_sent[peer]);
        k_sem_give(notification_slots[peer]);
    }
}
static int send_notification(struct bt_conn *host, const struct notification *n) {
    unsigned peer = n->peer;
    int64_t deadline = k_uptime_get() + 2000;
    while (k_sem_take(notification_slots[peer], K_MSEC(50))) {
        if (n->session != atomic_get(&host_generation[peer]))
            return -ENOTCONN;
        if (k_uptime_get() >= deadline)
            return -ETIMEDOUT;
    }
    if (n->session != atomic_get(&host_generation[peer])) {
        k_sem_give(notification_slots[peer]);
        return -ENOTCONN;
    }
    struct bt_gatt_notify_params p = {.attr = &attrs[n->index],
                                      .data = n->data,
                                      .len = n->length,
                                      .func = notification_sent,
                                      .user_data = (void *)(uintptr_t)((n->session << 1) | peer)};
    int e;
    while ((e = bt_gatt_notify_cb(host, &p)) == -ENOMEM && k_uptime_get() < deadline &&
           n->session == atomic_get(&host_generation[peer])) {
        k_sleep(K_MSEC(5));
    }
    if (e)
        k_sem_give(notification_slots[peer]);
    return e;
}
static void indication_destroy(struct bt_gatt_indicate_params *p) { k_sem_give(&indication_done); }
static void notify_thread(void *a, void *b, void *c) {
    unsigned peer = (uintptr_t)a;
    struct notification n;
    for (;;) {
        k_msgq_get(notification_queues[peer], &n, K_FOREVER);
        k_mutex_lock(notify_mutexes[peer], K_FOREVER);
        k_spinlock_key_t key = k_spin_lock(&host_lock);
        struct bt_conn *host = hosts[n.peer] ? bt_conn_ref(hosts[n.peer]) : NULL;
        k_spin_unlock(&host_lock, key);
        if (host && atomic_get(&active) && n.generation == atomic_get(&generation) &&
            n.session == atomic_get(&host_generation[n.peer]) &&
            atomic_get(&subscribed_values[n.peer][n.index])) {
            int e;
            if (n.kind & BT_GATT_CCC_NOTIFY)
                e = send_notification(host, &n);
            else {
                k_mutex_lock(&indication_mutex, K_FOREVER);
                struct bt_gatt_indicate_params p = {.attr = &attrs[n.index],
                                                    .data = n.data,
                                                    .len = n.length,
                                                    .destroy = indication_destroy};
                k_sem_reset(&indication_done);
                e = bt_gatt_indicate(host, &p);
                if (!e)
                    k_sem_take(&indication_done, K_FOREVER);
                k_mutex_unlock(&indication_mutex);
            }
            if (e) {
                atomic_set(&proxy_notify_last_error[peer], e);
                atomic_inc(&proxy_notify_errors[peer]);
                atomic_inc(&link_errors);
            } else if (!(n.kind & BT_GATT_CCC_NOTIFY))
                atomic_inc(&proxy_notify_sent[peer]);
        }
        if (host)
            bt_conn_unref(host);
        k_mutex_unlock(notify_mutexes[peer]);
    }
}
static void host_connected(struct bt_conn *conn, uint8_t err) {
    if (err)
        return;
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) || info.role != BT_CONN_ROLE_PERIPHERAL)
        return;
    k_spinlock_key_t key = k_spin_lock(&host_lock);
    for (unsigned i = 0; i < 2; i++)
        if (info.id == identities[i] && !hosts[i]) {
            atomic_inc(&host_generation[i]);
            k_sem_reset(notification_slots[i]);
            for (unsigned slot = 0; slot < 4; slot++)
                k_sem_give(notification_slots[i]);
            hosts[i] = bt_conn_ref(conn);
            printk("PROXY host ear=%u\n", i);
        }
    k_spin_unlock(&host_lock, key);
}
static void restart_ads(struct k_work *work) {
    for (unsigned i = 0; i < expected_peers; i++)
        if (advertisers[i] && !hosts[i] && !atomic_test_bit(&host_cleanup_pending, i))
            bt_le_ext_adv_start(advertisers[i], BT_LE_EXT_ADV_START_DEFAULT);
}
K_WORK_DEFINE(restart_work, restart_ads);
static void resume_advertising(void) { k_work_submit(&restart_work); }
static void host_disconnected(struct bt_conn *conn, uint8_t reason) {
    bool cleanup = false;
    k_spinlock_key_t key = k_spin_lock(&host_lock);
    for (unsigned i = 0; i < 2; i++)
        if (hosts[i] == conn) {
            atomic_inc(&host_generation[i]);
            for (unsigned j = 0; j < MAX_ATTRS; j++)
                atomic_set(&subscribed_values[i][j], 0);
            bt_conn_unref(hosts[i]);
            hosts[i] = NULL;
            atomic_set_bit(&host_cleanup_pending, i);
            cleanup = true;
        }
    k_spin_unlock(&host_lock, key);
    /* If the queue is full, its existing work will wake the worker anyway. */
    if (cleanup) {
        struct request wake = {0};
        k_msgq_put(&requests, &wake, K_NO_WAIT);
    }
}
BT_CONN_CB_DEFINE(proxy_callbacks) = {.connected = host_connected,
                                      .disconnected = host_disconnected};
int proxy_advertise(unsigned n) {
    expected_peers = n;
    atomic_set(&active, 1);
    const uint8_t info_uuid[] = {0x68, 0xb4, 0x96, 0x0f, 0x9b, 0x0b, 0x41, 0xb1,
                                 0x5a, 0x46, 0x68, 0x64, 0x10, 0x25, 0x62, 0x45};
    struct bt_data ad[] = {BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
                           BT_DATA(BT_DATA_UUID128_ALL, info_uuid, 16)};
    for (unsigned i = 0; i < n; i++) {
        struct bt_le_adv_param p = *BT_LE_ADV_CONN_FAST_2;
        /* Two continuously fast advertisers contend with bidirectional CIS
         * events. A one-second interval keeps the app discoverable while
         * reserving substantially more radio time for audio. */
        p.interval_min = BT_GAP_ADV_SLOW_INT_MIN;
        p.interval_max = BT_GAP_ADV_SLOW_INT_MAX;
        p.id = identities[i];
        p.options |= BT_LE_ADV_OPT_USE_IDENTITY;
        int e = bt_le_ext_adv_create(&p, NULL, &advertisers[i]);
        if (e)
            return e;
        struct bt_data sd =
            BT_DATA(BT_DATA_NAME_COMPLETE, ear_names[i], MIN(strlen(ear_names[i]), 29));
        e = bt_le_ext_adv_set_data(advertisers[i], ad, ARRAY_SIZE(ad), &sd, 1);
        if (e)
            return e;
        e = bt_le_ext_adv_start(advertisers[i], BT_LE_EXT_ADV_START_DEFAULT);
        if (e)
            return e;
    }
    return 0;
}
void proxy_reset(void) {
    atomic_set(&active, 0);
    atomic_inc(&generation);
    expected_peers = 0;
    k_work_cancel(&restart_work);
    for (unsigned i = 0; i < 2; i++) {
        if (advertisers[i]) {
            bt_le_ext_adv_stop(advertisers[i]);
            bt_le_ext_adv_delete(advertisers[i]);
            advertisers[i] = NULL;
        }
        if (hosts[i])
            bt_conn_disconnect(hosts[i], BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
    while (hosts[0] || hosts[1])
        k_sleep(K_MSEC(20));
    k_mutex_lock(&request_mutex, K_FOREVER);
    k_mutex_lock(notify_mutexes[0], K_FOREVER);
    k_mutex_lock(notify_mutexes[1], K_FOREVER);
    k_mutex_lock(&io_mutex, K_FOREVER);
    for (unsigned i = 0; i < 2; i++) {
        if (ears[i]) {
            bt_conn_unref(ears[i]);
            ears[i] = NULL;
        }
    }
    for (unsigned i = 0; i < service_count; i++)
        bt_gatt_service_unregister(&services[i]);
    service_count = attr_count = char_count = 0;
    memset(maps, 0, sizeof(maps));
    memset(attrs, 0, sizeof(attrs));
    memset(subscriptions, 0, sizeof(subscriptions));
    memset(subscription_count, 0, sizeof(subscription_count));
    memset(subscribed_values, 0, sizeof(subscribed_values));
    k_mutex_unlock(&io_mutex);
    k_mutex_unlock(notify_mutexes[1]);
    k_mutex_unlock(notify_mutexes[0]);
    k_mutex_unlock(&request_mutex);
}
static K_THREAD_STACK_DEFINE(request_stack, 4096);
static K_THREAD_STACK_ARRAY_DEFINE(notify_stacks, 2, 2048);
static struct k_thread request_task, notify_tasks[2];
void proxy_init(void) {
    size_t count = CONFIG_BT_ID_MAX;
    bt_addr_le_t ids[CONFIG_BT_ID_MAX];
    bt_id_get(ids, &count);
    for (unsigned i = count; i < 3; i++)
        bt_id_create(NULL, NULL);
    identities[0] = 1;
    identities[1] = 2;
    k_thread_create(&request_task, request_stack, K_THREAD_STACK_SIZEOF(request_stack),
                    request_thread, NULL, NULL, NULL, 8, 0, K_NO_WAIT);
    for (unsigned i = 0; i < 2; i++)
        k_thread_create(&notify_tasks[i], notify_stacks[i],
                        K_THREAD_STACK_SIZEOF(notify_stacks[i]), notify_thread,
                        (void *)(uintptr_t)i, NULL, NULL, 8, 0, K_NO_WAIT);
}
