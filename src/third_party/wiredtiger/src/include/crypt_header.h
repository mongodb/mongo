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
WT_PACKED_STRUCT_BEGIN(__wt_crypt_header)
/*
 * Signature 'wtch' (WiredTiger Crypt Header)
 */
#define WT_CRYPT_HEADER_SIGNATURE 0x68637477u
    uint32_t signature; /* 00-03: Key header signature; always 'wtch' */
#define WT_CRYPT_HEADER_VERSION 1u
    uint8_t version;     /* 04: Header version */
    uint8_t header_size; /* 05: Header size, in bytes */
    uint32_t crypt_size; /* 06-09: Payload size, in bytes */
    uint32_t checksum;   /* 10-13: Payload CRC32 checksum */
WT_PACKED_STRUCT_END

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
    hdr->checksum = __wt_bswap32(hdr->checksum);
#else
    WT_UNUSED(hdr);
#endif
}
