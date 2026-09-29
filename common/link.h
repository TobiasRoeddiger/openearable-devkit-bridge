/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define OE_LINK_MAGIC 0x3141454fu /* OEA1, little endian */
#define OE_LINK_VERSION 2
#define OE_HOST_CONNECTED 1u
#define OE_HOST_MIC_ACTIVE 2u
#define OE_HOST_PLAYING 4u
#define OE_EAR_CONNECTED 8u
#define OE_EAR_STREAMING 16u
#define OE_LINK_TEST 32u
#define OE_EAR_PAIRING 64u
#define OE_NAME_VALID 128u
#define OE_HOST_PAIRABLE 256u
/* Fixed full-duplex SPI transaction. A response describes the previous request.
 * All counters wrap naturally; CRC covers padding, too. Never send C pointers. */
struct __attribute__((packed)) oe_link_frame {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t sequence;
    uint32_t flags;
    uint32_t pairing_token;
    uint32_t pcm_frames;
    uint32_t errors;
    uint32_t peak;
    char name[64];
    uint32_t reset_epoch;
    uint32_t boot_id;
    char hardware_revision[16];
    uint8_t reserved[4];
    uint32_t crc;
};
static inline uint32_t oe_crc32(const void *ptr, size_t n) {
    const uint8_t *p = (const uint8_t *)ptr;
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (unsigned b = 0; b < 8; b++)
            c = (c >> 1) ^ ((0u - (c & 1u)) & 0xedb88320u);
    }
    return ~c;
}
static inline void oe_link_seal(struct oe_link_frame *f) {
    f->magic = OE_LINK_MAGIC;
    f->version = OE_LINK_VERSION;
    f->size = sizeof(*f);
    f->crc = oe_crc32(f, offsetof(struct oe_link_frame, crc));
}
static inline int oe_link_valid(const struct oe_link_frame *f) {
    return f->magic == OE_LINK_MAGIC && f->version == OE_LINK_VERSION && f->size == sizeof(*f) &&
           f->crc == oe_crc32(f, offsetof(struct oe_link_frame, crc));
}
