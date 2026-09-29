/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <zephyr/bluetooth/conn.h>
#include <stddef.h>
#include <zephyr/bluetooth/uuid.h>
void proxy_init(void);
int proxy_attach(unsigned peer, struct bt_conn *conn, char *name, size_t size);
void proxy_discard_pending(unsigned peer);
int proxy_advertise(unsigned peers);
void proxy_reset(void);
int proxy_read_uuid(struct bt_conn *conn, const struct bt_uuid *uuid, void *data, size_t size);
int proxy_set_render_volume(unsigned peer, struct bt_conn *conn, uint8_t volume);
