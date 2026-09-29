/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The Classic Device ID version field is a 16-bit BCD value: JJ.M.N.
 * Keep the original hardware string on BLE; do not truncate versions that
 * cannot be represented by this separate Classic field. */
static inline bool oe_hardware_version_bcd(const char *text, uint16_t *version) {
    unsigned parts[3] = {0};
    for (unsigned part = 0; part < 3; part++) {
        if (*text < '0' || *text > '9')
            return false;
        do {
            parts[part] = parts[part] * 10 + (*text++ - '0');
            if (parts[part] > (part ? 9u : 99u))
                return false;
        } while (*text >= '0' && *text <= '9');
        if (part < 2 && *text++ != '.')
            return false;
    }
    if (*text)
        return false;
    *version = ((parts[0] / 10) << 12) | ((parts[0] % 10) << 8) | (parts[1] << 4) | parts[2];
    return true;
}
