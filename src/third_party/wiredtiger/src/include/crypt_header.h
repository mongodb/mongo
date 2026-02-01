/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * WT_CRYPT_HEADER --
 *	Header for encryption key data.
 */
struct __wt_crypt_header {
/*
 * Signature 'wtch' (WiredTiger Crypt Header)
 */
#define WT_CRYPT_HEADER_SIGNATURE 0x68637477u
    uint32_t signature; /* 00-03: Key header signature; always 'wtch' */

    /*
     * As we create new versions, bump the version number here, and consider what previous versions
     * are compatible with it.
     */
#define WT_CRYPT_HEADER_VERSION 0x1u
    uint8_t version; /* 04: Header version */
#define WT_CRYPT_HEADER_COMPATIBLE_VERSION 0x1u
    uint8_t compatible_version; /* 05: Minimum compatibility version */
    uint8_t header_size;        /* 06: Header size, in bytes */
    uint8_t unused[1];          /* 07: Unused padding */
    uint32_t crypt_size;        /* 08-11: Payload size, in bytes */
    uint32_t checksum;          /* 12-15: Payload CRC32 checksum */
};

/*
 * __wt_crypt_header_byteswap --
 *     Handle big- and little-endian transformation of a key header.
 */
static WT_INLINE void
__wt_crypt_header_byteswap(WT_CRYPT_HEADER *hdr)
{
#ifdef WORDS_BIGENDIAN
    hdr->signature = __wt_bswap32(hdr->signature);
    hdr->crypt_size = __wt_bswap32(hdr->crypt_size);
#else
    WT_UNUSED(hdr);
#endif
}
