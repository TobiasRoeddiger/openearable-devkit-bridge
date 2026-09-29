"""Add an asynchronous proxy hook to a build-local copy of NCS 3.0.1 ATT.

Never modifies the SDK. Only proxy reads/writes are intercepted; all normal
ATT traffic, discovery, security and LE Audio use the unmodified host paths.
"""
from pathlib import Path
import sys
source=Path(sys.argv[1]).read_text()
needle='static int bt_att_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)\n{'
assert source.count(needle)==1, 'Unsupported Zephyr ATT version'
source=source.replace(needle, '''extern bool adapter_att_request(struct bt_conn *, const uint8_t *, size_t);
int adapter_att_reply(struct bt_conn *conn, uint8_t op, const void *data, size_t len)
{
    struct bt_l2cap_chan *base=bt_l2cap_le_lookup_rx_cid(conn, BT_L2CAP_CID_ATT);
    if (!base || conn->state != BT_CONN_CONNECTED) return -ENOTCONN;
    struct bt_att_chan *chan=ATT_CHAN(base);
    struct net_buf *pdu=bt_att_chan_create_pdu(chan,op,len);
    if (!pdu) return -ENOMEM;
    net_buf_add_mem(pdu,data,len);
    bt_att_chan_send_rsp(chan,pdu);
    return 0;
}
''' + needle + '''
    if (chan->conn && adapter_att_request(chan->conn, buf->data, buf->len)) return 0;
''')
Path(sys.argv[2]).write_text(source)
