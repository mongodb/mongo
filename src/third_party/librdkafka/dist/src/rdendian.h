/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2015 Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef _RDENDIAN_H_
#define _RDENDIAN_H_

/**
 * Provides portable endian-swapping macros/functions.
 *
 *   be64toh()
 *   htobe64()
 *   be32toh()
 *   htobe32()
 *   be16toh()
 *   htobe16()
 *   le64toh()
 */

#ifdef __FreeBSD__
#include <sys/endian.h>
#elif defined __GLIBC__
#include <endian.h>
#ifndef be64toh
/* Support older glibc (<2.9) which lack be64toh */
#include <byteswap.h>
#if __BYTE_ORDER == __BIG_ENDIAN
#define be16toh(x) (x)
#define be32toh(x) (x)
#define be64toh(x) (x)
#define le64toh(x) __bswap_64(x)
#define le32toh(x) __bswap_32(x)
#else
#define be16toh(x) __bswap_16(x)
#define be32toh(x) __bswap_32(x)
#define be64toh(x) __bswap_64(x)
#define le64toh(x) (x)
#define le32toh(x) (x)
#endif
#endif

#elif defined __CYGWIN__
#include <endian.h>
#elif defined __BSD__
#include <sys/endian.h>
#elif defined __sun
#include <sys/byteorder.h>
#include <sys/isa_defs.h>
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#ifdef _BIG_ENDIAN
#define __BYTE_ORDER __BIG_ENDIAN
#define be64toh(x)   (x)
#define be32toh(x)   (x)
#define be16toh(x)   (x)
#define le16toh(x)   ((uint16_t)BSWAP_16(x))
#define le32toh(x)   BSWAP_32(x)
#define le64toh(x)   BSWAP_64(x)
#else
#define __BYTE_ORDER __LITTLE_ENDIAN
#define be64toh(x)   BSWAP_64(x)
#define be32toh(x)   ntohl(x)
#define be16toh(x)   ntohs(x)
#define le16toh(x)   (x)
#define le32toh(x)   (x)
#define le64toh(x)   (x)
#define htole16(x)   (x)
#define htole64(x)   (x)
#endif /* __sun */

#elif defined __APPLE__
#include <machine/endian.h>
#include <libkern/OSByteOrder.h>
#if __DARWIN_BYTE_ORDER == __DARWIN_BIG_ENDIAN
#define be64toh(x) (x)
#define be32toh(x) (x)
#define be16toh(x) (x)
#define le16toh(x) OSSwapInt16(x)
#define le32toh(x) OSSwapInt32(x)
#define le64toh(x) OSSwapInt64(x)
#else
#define be64toh(x) OSSwapInt64(x)
#define be32toh(x) OSSwapInt32(x)
#define be16toh(x) OSSwapInt16(x)
#define le16toh(x) (x)
#define le32toh(x) (x)
#define le64toh(x) (x)
#endif

#elif defined(_WIN32)
#include <intrin.h>

#define be64toh(x) _byteswap_uint64(x)
#define be32toh(x) _byteswap_ulong(x)
#define be16toh(x) _byteswap_ushort(x)
#define le16toh(x) (x)
#define le32toh(x) (x)
#define le64toh(x) (x)

#elif defined _AIX /* AIX is always big endian */
#define be64toh(x) (x)
#define be32toh(x) (x)
#define be16toh(x) (x)
#define le32toh(x)                                                             \
        ((((x)&0xff) << 24) | (((x)&0xff00) << 8) | (((x)&0xff0000) >> 8) |    \
         (((x)&0xff000000) >> 24))
#define le64toh(x)                                                             \
        ((((x)&0x00000000000000ffL) << 56) |                                   \
         (((x)&0x000000000000ff00L) << 40) |                                   \
         (((x)&0x0000000000ff0000L) << 24) |                                   \
         (((x)&0x00000000ff000000L) << 8) | (((x)&0x000000ff00000000L) >> 8) | \
         (((x)&0x0000ff0000000000L) >> 24) |                                   \
         (((x)&0x00ff000000000000L) >> 40) |                                   \
         (((x)&0xff00000000000000L) >> 56))
#else
#include <endian.h>
#endif



/*
 * On Solaris, be64toh is a function, not a macro, so there's no need to error
 * if it's not defined.
 */
#if !defined(__sun) && !defined(be64toh)
#error Missing definition for be64toh
#endif

#ifndef be32toh
#define be32toh(x) ntohl(x)
#endif

#ifndef be16toh
#define be16toh(x) ntohs(x)
#endif

#ifndef htobe64
#define htobe64(x) be64toh(x)
#endif
#ifndef htobe32
#define htobe32(x) be32toh(x)
#endif
#ifndef htobe16
#define htobe16(x) be16toh(x)
#endif

#ifndef htole32
#define htole32(x) le32toh(x)
#endif

#endif /* _RDENDIAN_H_ */
