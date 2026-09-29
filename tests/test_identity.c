/* SPDX-License-Identifier: Apache-2.0 */
#include "hardware_version.h"
#include "link.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    const struct {
        const char *text;
        uint16_t bcd;
    } valid[] = {
        {"2.0.1", 0x0201},
        {"3.1.4", 0x0314},
        {"12.3.4", 0x1234},
        {"99.9.9", 0x9999},
    };
    for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        uint16_t bcd = 0;
        assert(oe_hardware_version_bcd(valid[i].text, &bcd));
        assert(bcd == valid[i].bcd);
    }
    const char *invalid[] = {"",        "2",      "2.0",    "2..1",   "2.0.1-extra", "2.0.1.2",
                             "100.0.0", "2.10.1", "2.0.10", "-2.0.1", "v2.0.1"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        uint16_t bcd = 0xbeef;
        assert(!oe_hardware_version_bcd(invalid[i], &bcd));
        assert(bcd == 0xbeef);
    }
    struct oe_link_frame frame = {0};
    strcpy(frame.name, "OpenEarable-Test");
    strcpy(frame.hardware_revision, "2.0.1");
    frame.flags = OE_NAME_VALID;
    oe_link_seal(&frame);
    assert(sizeof(frame) == 128 && oe_link_valid(&frame));
    frame.hardware_revision[0] = '3';
    assert(!oe_link_valid(&frame));
    oe_link_seal(&frame);
    assert(oe_link_valid(&frame));
    frame.version = 1;
    frame.crc = oe_crc32(&frame, offsetof(struct oe_link_frame, crc));
    assert(!oe_link_valid(&frame));
    puts("Hardware version conversion and SPI identity integrity passed");
}
