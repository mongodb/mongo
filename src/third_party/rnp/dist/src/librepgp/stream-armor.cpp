/*
 * Copyright (c) 2017-2023, [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1.  Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 * 2.  Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <stdlib.h>
#include <stdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#else
#include "uniwin.h"
#endif
#include <string>
#include <algorithm>
#include <cassert>
#include "stream-def.h"
#include "stream-armor.h"
#include "stream-packet.h"
#include "str-utils.h"
#include "crypto/hash.hpp"
#include "utils.h"

#define ARMORED_BLOCK_SIZE (4096)
#define ARMORED_PEEK_BUF_SIZE 1024
#define ARMORED_MIN_LINE_LENGTH (16)
#define ARMORED_MAX_LINE_LENGTH (76)

typedef struct pgp_source_armored_param_t {
    pgp_source_t *    readsrc;  /* source to read from */
    pgp_armored_msg_t type;     /* type of the message */
    std::string       armorhdr; /* minimized header, i.e. PUBLIC KEY FILE, MESSAGE, etc */
    uint8_t  rest[ARMORED_BLOCK_SIZE]; /* unread decoded bytes, makes implementation easier */
    unsigned restlen;                  /* number of bytes in rest */
    unsigned restpos;    /* index of first unread byte in rest, restpos <= restlen */
    uint8_t  brest[3];   /* decoded 6-bit tail bytes */
    unsigned brestlen;   /* number of bytes in brest */
    bool     eofb64;     /* end of base64 stream reached */
    uint8_t  readcrc[3]; /* crc-24 from the armored data */
    bool     has_crc;    /* message contains CRC line */
    std::unique_ptr<rnp::CRC24> crc_ctx;   /* CTX used to calculate CRC */
    bool                        noheaders; /* only base64 data, no headers */
} pgp_source_armored_param_t;

typedef struct pgp_dest_armored_param_t {
    pgp_dest_t *      writedst;
    pgp_armored_msg_t type;    /* type of the message */
    char              eol[2];  /* end of line, all non-zeroes are written */
    unsigned          lout;    /* chars written in current line */
    unsigned          llen;    /* length of the base64 line, defaults to 76 as per RFC */
    uint8_t           tail[2]; /* bytes which didn't fit into 3-byte boundary */
    unsigned          tailc;   /* number of bytes in tail */
    std::unique_ptr<rnp::CRC24> crc_ctx; /* CTX used to calculate CRC */
} pgp_dest_armored_param_t;

/*
   Table for base64 lookups:
   0xff - wrong character,
   0xfe - '='
   0xfd - eol/whitespace,
   0..0x3f - represented 6-bit number
*/
static const uint8_t B64DEC[256] = {
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfd, 0xfd, 0xff, 0xff, 0xfd, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3e, 0xff,
  0xff, 0xff, 0x3f, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0xff, 0xff,
  0xff, 0xfe, 0xff, 0xff, 0xff, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
  0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
  0x19, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21,
  0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
  0x31, 0x32, 0x33, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff};

static bool
armor_read_padding(pgp_source_t &src, size_t *read)
{
    char   st[64];
    size_t stlen = 0;

    if (!src.peek_line(st, 64, &stlen)) {
        return false;
    }

    if ((stlen == 1) || (stlen == 2)) {
        if ((st[0] != CH_EQ) || ((stlen == 2) && (st[1] != CH_EQ))) {
            return false;
        }

        *read = stlen;
        src.skip(stlen);
        return src.skip_eol();
    } else if (stlen == 5) {
        *read = 0;
        return true;
    } else if ((stlen > 5) && !memcmp(st, ST_DASHES, 5)) {
        /* case with absent crc and 3-byte last chunk */
        *read = 0;
        return true;
    }
    return false;
}

static bool
base64_read_padding(pgp_source_t &src, size_t *read)
{
    char   pad[16];
    size_t padlen = sizeof(pad);

    /* we would allow arbitrary number of whitespaces/eols after the padding */
    if (!src.read(pad, padlen, &padlen)) {
        return false;
    }
    /* strip trailing whitespaces */
    while (padlen && (B64DEC[(int) pad[padlen - 1]] == 0xfd)) {
        padlen--;
    }
    /* check for '=' */
    for (size_t i = 0; i < padlen; i++) {
        if (pad[i] != CH_EQ) {
            RNP_LOG("wrong base64 padding: %.*s", (int) padlen, pad);
            return false;
        }
    }
    if (padlen > 2) {
        RNP_LOG("wrong base64 padding length %zu.", padlen);
        return false;
    }
    if (!src.eof()) {
        RNP_LOG("warning: extra data after the base64 stream.");
    }
    *read = padlen;
    return true;
}

static bool
armor_read_crc(pgp_source_armored_param_t *param)
{
    uint8_t dec[4] = {0};
    char    crc[8] = {0};
    size_t  clen = 0;

    if (!param->readsrc->peek_line(crc, sizeof(crc), &clen)) {
        return false;
    }

    if ((clen != 5) || (crc[0] != CH_EQ)) {
        return false;
    }

    for (int i = 0; i < 4; i++) {
        if ((dec[i] = B64DEC[(uint8_t) crc[i + 1]]) >= 64) {
            return false;
        }
    }

    param->readcrc[0] = (dec[0] << 2) | ((dec[1] >> 4) & 0x0F);
    param->readcrc[1] = (dec[1] << 4) | ((dec[2] >> 2) & 0x0F);
    param->readcrc[2] = (dec[2] << 6) | dec[3];

    param->has_crc = true;

    param->readsrc->skip(5);
    return param->readsrc->skip_eol();
}

static bool
armor_read_trailer(pgp_source_armored_param_t *param)
{
    /* Space or tab could get between armor and trailer, see issue #2199 */
    if (!param->readsrc->skip_chars("\r\n \t")) {
        return false;
    }

    std::string st = ST_ARMOR_END + param->armorhdr + ST_DASHES;
    std::string str(st.size(), 0);
    if (!param->readsrc->peek_eq(&str.front(), str.size()) || (st != str)) {
        return false;
    }
    param->readsrc->skip(st.size());
    (void) param->readsrc->skip_chars("\t ");
    (void) param->readsrc->skip_eol();
    return true;
}

static bool
armored_update_crc(pgp_source_armored_param_t *param,
                   const void *                buf,
                   size_t                      len,
                   bool                        finish = false)
{
    if (param->noheaders) {
        return true;
    }
    try {
        param->crc_ctx->add(buf, len);
        if (!finish) {
            return true;
        }
        auto crc = param->crc_ctx->finish();
        if (param->has_crc && memcmp(param->readcrc, crc.data(), 3)) {
            RNP_LOG("Warning: CRC mismatch");
        }
        return true;
    } catch (const std::exception &e) {
        RNP_LOG("%s", e.what());
        return false;
    }
}

static bool
armored_src_read(pgp_source_t *src, void *buf, size_t len, size_t *readres)
{
    pgp_source_armored_param_t *param = (pgp_source_armored_param_t *) src->param;
    uint8_t  b64buf[ARMORED_BLOCK_SIZE];     /* input base64 data with spaces and so on */
    uint8_t  decbuf[ARMORED_BLOCK_SIZE + 4]; /* decoded 6-bit values */
    uint8_t *bufptr = (uint8_t *) buf;       /* for better readability below */
    uint8_t *bptr, *bend;                    /* pointer to input data in b64buf */
    uint8_t *dptr, *dend, *pend; /* pointers to decoded data in decbuf: working pointer, last
                                    available byte, last byte to process */
    uint8_t  bval;
    uint32_t b24;
    size_t   read = 0;
    size_t   left = len;
    size_t   eqcount = 0; /* number of '=' at the end of base64 stream */

    if (!param) {
        return false;
    }

    /* checking whether there are some decoded bytes */
    if (param->restpos < param->restlen) {
        if (param->restlen - param->restpos >= len) {
            memcpy(bufptr, &param->rest[param->restpos], len);
            param->restpos += len;
            try {
                param->crc_ctx->add(bufptr, len);
            } catch (const std::exception &e) {
                RNP_LOG("%s", e.what());
                return false;
            }
            *readres = len;
            return true;
        } else {
            left = len - (param->restlen - param->restpos);
            memcpy(bufptr, &param->rest[param->restpos], len - left);
            param->restpos = param->restlen = 0;
            bufptr += len - left;
        }
    }

    if (param->eofb64) {
        *readres = len - left;
        return true;
    }

    memcpy(decbuf, param->brest, param->brestlen);
    dend = decbuf + param->brestlen;

    do {
        if (!param->readsrc->peek(b64buf, sizeof(b64buf), &read)) {
            return false;
        }
        if (!read) {
            RNP_LOG("premature end of armored input");
            return false;
        }

        dptr = dend;
        bptr = b64buf;
        bend = b64buf + read;
        /* checking input data, stripping away whitespaces, checking for end of the b64 data */
        while (bptr < bend) {
            if ((bval = B64DEC[*(bptr++)]) < 64) {
                *(dptr++) = bval;
            } else if (bval == 0xfe) {
                /* '=' means the base64 padding or the beginning of checksum */
                param->eofb64 = true;
                break;
            } else if (bval == 0xff) {
                auto ch = *(bptr - 1);
                /* OpenPGP message headers without the crc and without trailing = */
                if ((ch == CH_DASH) && !param->noheaders) {
                    param->eofb64 = true;
                    break;
                }
                RNP_LOG("wrong base64 character 0x%02hhX", ch);
                return false;
            }
        }

        dend = dptr;
        dptr = decbuf;
        /* Processing full 4s which will go directly to the buf.
           After this left < 3 or decbuf has < 4 bytes */
        if ((size_t)(dend - dptr) / 4 * 3 < left) {
            pend = decbuf + (dend - dptr) / 4 * 4;
            left -= (dend - dptr) / 4 * 3;
        } else {
            pend = decbuf + (left / 3) * 4;
            left -= left / 3 * 3;
        }

        /* this one would the most performance-consuming part for large chunks */
        while (dptr < pend) {
            b24 = *dptr++ << 18;
            b24 |= *dptr++ << 12;
            b24 |= *dptr++ << 6;
            b24 |= *dptr++;
            *bufptr++ = b24 >> 16;
            *bufptr++ = b24 >> 8;
            *bufptr++ = b24 & 0xff;
        }

        /* moving rest to the beginning of decbuf */
        memmove(decbuf, dptr, dend - dptr);
        dend = decbuf + (dend - dptr);

        /* skip already processed data */
        if (!param->eofb64) {
            /* all input is base64 data or eol/spaces, so skipping it */
            param->readsrc->skip(read);
            /* check for eof for base64-encoded data without headers */
            if (param->noheaders && param->readsrc->eof()) {
                param->readsrc->skip(read);
                param->eofb64 = true;
            } else {
                continue;
            }
        } else {
            /* '=' reached, bptr points on it */
            param->readsrc->skip(bptr - b64buf - 1);
        }

        /* end of base64 data */
        if (param->noheaders) {
            if (!base64_read_padding(*param->readsrc, &eqcount)) {
                return false;
            }
            break;
        }
        /* reading b64 padding if any */
        if (!armor_read_padding(*param->readsrc, &eqcount)) {
            RNP_LOG("wrong padding");
            return false;
        }
        /* reading crc */
        if (!armor_read_crc(param)) {
            RNP_LOG("Warning: missing or malformed CRC line");
        }
        /* reading armor trailing line */
        if (!armor_read_trailer(param)) {
            RNP_LOG("wrong armor trailer");
            return false;
        }
        break;
    } while (left >= 3);

    /* process bytes left in decbuf */

    dptr = decbuf;
    pend = decbuf + (dend - decbuf) / 4 * 4;
    bptr = param->rest;
    while (dptr < pend) {
        b24 = *dptr++ << 18;
        b24 |= *dptr++ << 12;
        b24 |= *dptr++ << 6;
        b24 |= *dptr++;
        *bptr++ = b24 >> 16;
        *bptr++ = b24 >> 8;
        *bptr++ = b24 & 0xff;
    }

    if (!armored_update_crc(param, buf, bufptr - (uint8_t *) buf)) {
        return false;
    }

    if (param->eofb64) {
        if ((dend - dptr + eqcount) % 4 != 0) {
            RNP_LOG("wrong b64 padding");
            return false;
        }

        if (eqcount == 1) {
            b24 = (*dptr << 10) | (*(dptr + 1) << 4) | (*(dptr + 2) >> 2);
            *bptr++ = b24 >> 8;
            *bptr++ = b24 & 0xff;
        } else if (eqcount == 2) {
            *bptr++ = (*dptr << 2) | (*(dptr + 1) >> 4);
        }

        /* Calculate CRC after reading whole input stream */
        if (!armored_update_crc(param, param->rest, bptr - param->rest, true)) {
            return false;
        }
    } else {
        /* few bytes which do not fit to 4 boundary */
        for (int i = 0; i < dend - dptr; i++) {
            param->brest[i] = *(dptr + i);
        }
        param->brestlen = dend - dptr;
    }

    param->restlen = bptr - param->rest;

    /* check whether we have some bytes to add */
    if ((left > 0) && (param->restlen > 0)) {
        read = left > param->restlen ? param->restlen : left;
        memcpy(bufptr, param->rest, read);
        if (!param->eofb64 && !armored_update_crc(param, bufptr, read)) {
            return false;
        }
        left -= read;
        param->restpos += read;
    }

    *readres = len - left;
    return true;
}

static void
armored_src_close(pgp_source_t *src)
{
    auto param = (pgp_source_armored_param_t *) src->param;
    delete param;
    src->param = NULL;
}

/** @brief finds armor header position in the buffer, returning position of header or
           *std::string::npos. hdrlen will contain the length of the header.
           type will contain content type, i.e. MESSAGE, PUBLIC KEY, whatever.
 **/
static size_t
find_armor_header(const std::string &str, size_t &hdrlen, std::string &type)
{
    size_t bg_len = strlen(ST_ARMOR_BEGIN);
    size_t pos = str.find(ST_ARMOR_BEGIN);
    if (pos == std::string::npos) {
        return pos;
    }
    size_t end = str.find(ST_DASHES, pos + bg_len);
    if (end == std::string::npos) {
        return end;
    }
    hdrlen = end + 5;
    type.assign(str.begin() + pos + bg_len, str.begin() + end);
    return pos;
}

static pgp_armored_msg_t
armor_str_to_data_type(const std::string &str)
{
    if ((str == "MESSAGE") || (str == "ARMORED FILE")) {
        return PGP_ARMORED_MESSAGE;
    }
    if ((str == "PUBLIC KEY BLOCK") || (str == "PUBLIC KEY")) {
        return PGP_ARMORED_PUBLIC_KEY;
    }
    if ((str == "SECRET KEY BLOCK") || (str == "SECRET KEY") || (str == "PRIVATE KEY BLOCK") ||
        (str == "PRIVATE KEY")) {
        return PGP_ARMORED_SECRET_KEY;
    }
    if (str == "SIGNATURE") {
        return PGP_ARMORED_SIGNATURE;
    }
    return PGP_ARMORED_UNKNOWN;
}

pgp_armored_msg_t
rnp_armor_guess_type(pgp_source_t *src)
{
    uint8_t ptag;

    if (!src->peek_eq(&ptag, 1)) {
        return PGP_ARMORED_UNKNOWN;
    }

    switch (get_packet_type(ptag)) {
    case PGP_PKT_PK_SESSION_KEY:
    case PGP_PKT_SK_SESSION_KEY:
    case PGP_PKT_ONE_PASS_SIG:
    case PGP_PKT_SE_DATA:
    case PGP_PKT_SE_IP_DATA:
    case PGP_PKT_COMPRESSED:
    case PGP_PKT_LITDATA:
    case PGP_PKT_MARKER:
        return PGP_ARMORED_MESSAGE;
    case PGP_PKT_PUBLIC_KEY:
    case PGP_PKT_PUBLIC_SUBKEY:
        return PGP_ARMORED_PUBLIC_KEY;
    case PGP_PKT_SECRET_KEY:
    case PGP_PKT_SECRET_SUBKEY:
        return PGP_ARMORED_SECRET_KEY;
    case PGP_PKT_SIGNATURE:
        return PGP_ARMORED_SIGNATURE;
    default:
        return PGP_ARMORED_UNKNOWN;
    }
}

static pgp_armored_msg_t
rnp_armored_guess_type_by_readahead(pgp_source_t *src)
{
    if (!src->cache) {
        return PGP_ARMORED_UNKNOWN;
    }

    pgp_source_t armorsrc = {0};
    pgp_source_t memsrc = {0};
    size_t       read;
    // peek as much as the cache can take
    bool cache_res = src->peek(NULL, sizeof(src->cache->buf), &read);
    if (!cache_res || !read ||
        init_mem_src(&memsrc,
                     src->cache->buf + src->cache->pos,
                     src->cache->len - src->cache->pos,
                     false)) {
        return PGP_ARMORED_UNKNOWN;
    }
    rnp_result_t res = init_armored_src(&armorsrc, &memsrc);
    if (res) {
        memsrc.close();
        RNP_LOG("failed to parse armored data");
        return PGP_ARMORED_UNKNOWN;
    }
    pgp_armored_msg_t guessed = rnp_armor_guess_type(&armorsrc);
    armorsrc.close();
    memsrc.close();
    return guessed;
}

static std::string
peek_armor_header(pgp_source_t &src, size_t *skip, bool check)
{
    std::string hdr(ARMORED_PEEK_BUF_SIZE, '\0');
    size_t      read = 0;
    if (skip) {
        *skip = 0;
    }
    if (!src.peek(&hdr.front(), hdr.size(), &read) || (read < 20)) {
        return "";
    }
    hdr.resize(read);

    std::string type;
    size_t      armhdrlen = 0;
    size_t      pos = find_armor_header(hdr, armhdrlen, type);
    if (pos == std::string::npos) {
        return "";
    }

    /* if there are non-whitespaces before the armor header then issue warning */
    for (size_t idx = 0; check && (idx < pos); idx++) {
        if (B64DEC[(uint8_t) hdr[idx]] != 0xfd) {
            RNP_LOG("extra data before the header line");
            break;
        }
    }

    if (skip) {
        *skip = armhdrlen;
    }
    return type;
}

pgp_armored_msg_t
rnp_armored_get_type(pgp_source_t *src)
{
    auto guessed = rnp_armored_guess_type_by_readahead(src);
    if (guessed != PGP_ARMORED_UNKNOWN) {
        return guessed;
    }
    return armor_str_to_data_type(peek_armor_header(*src, nullptr, false));
}

static bool
armor_parse_header(pgp_source_armored_param_t *param)
{
    size_t      skip = 0;
    std::string hdr = peek_armor_header(*param->readsrc, &skip, true);
    if (hdr.empty()) {
        RNP_LOG("no armor header");
        return false;
    }

    param->type = armor_str_to_data_type(hdr);
    if (param->type == PGP_ARMORED_UNKNOWN) {
        RNP_LOG("unknown armor header");
        return false;
    }

    param->armorhdr = std::move(hdr);
    param->readsrc->skip(skip);
    param->readsrc->skip_chars("\t ");
    return true;
}

static bool
armor_skip_line(pgp_source_t &src)
{
    char header[ARMORED_PEEK_BUF_SIZE] = {0};
    do {
        size_t hdrlen = 0;
        bool   res = src.peek_line(header, sizeof(header), &hdrlen);
        if (hdrlen) {
            src.skip(hdrlen);
        }
        if (res || (hdrlen < sizeof(header) - 1)) {
            return res;
        }
    } while (1);
}

static bool
is_base64_line(const char *line, size_t len)
{
    for (size_t i = 0; i < len && line[i]; i++) {
        if (B64DEC[(uint8_t) line[i]] == 0xff)
            return false;
    }
    return true;
}

static bool
armor_header_known(const std::string &name)
{
    return (name == ST_HEADER_VERSION) || (name == ST_HEADER_COMMENT) ||
           (name == ST_HEADER_CHARSET) || (name == ST_HEADER_HASH);
}

static bool
armor_parse_headers(pgp_source_armored_param_t *param)
{
    std::vector<char> header(ARMORED_PEEK_BUF_SIZE, '\0');

    do {
        size_t hdrlen = 0;
        if (!param->readsrc->peek_line(header.data(), header.size(), &hdrlen)) {
            /* if line is too long let's cut it to the reasonable size */
            param->readsrc->skip(hdrlen);
            if ((hdrlen != header.size() - 1) || !armor_skip_line(*param->readsrc)) {
                RNP_LOG("failed to peek line: unexpected end of data");
                return false;
            }
            RNP_LOG("Too long armor header - truncated.");
        } else if (hdrlen) {
            if (is_base64_line(header.data(), hdrlen)) {
                RNP_LOG("Warning: no empty line after the base64 headers");
                return true;
            }
            param->readsrc->skip(hdrlen);
            if (rnp::is_blank_line(header.data(), hdrlen)) {
                return param->readsrc->skip_eol();
            }
        } else {
            /* empty line - end of the headers */
            return param->readsrc->skip_eol();
        }

        std::string hdrst(header.begin(), header.begin() + hdrlen);
        size_t      pos = hdrst.find(": ");
        if ((pos != std::string::npos) && armor_header_known(hdrst.substr(0, pos + 2))) {
            // do nothing at the moment, just check whether header is known
        } else {
            RNP_LOG("unknown header '%s'", hdrst.c_str());
        }

        if (!param->readsrc->skip_eol()) {
            return false;
        }
    } while (1);
}

rnp_result_t
init_armored_src(pgp_source_t *src, pgp_source_t *readsrc, bool noheaders)
{
    if (!init_src_common(src, 0)) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    pgp_source_armored_param_t *param = new (std::nothrow) pgp_source_armored_param_t();
    if (!param) {
        RNP_LOG("allocation failed");
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    param->readsrc = readsrc;
    param->noheaders = noheaders;
    src->param = param;
    src->raw_read = armored_src_read;
    src->raw_close = armored_src_close;
    src->type = PGP_STREAM_ARMORED;

    /* base64 data only */
    if (noheaders) {
        return RNP_SUCCESS;
    }

    /* initialize crc context */
    param->crc_ctx = rnp::CRC24::create();
    /* parsing armored header */
    rnp_result_t errcode = RNP_ERROR_GENERIC;
    if (!armor_parse_header(param)) {
        errcode = RNP_ERROR_BAD_FORMAT;
        goto finish;
    }
    /* eol */
    if (!param->readsrc->skip_eol()) {
        RNP_LOG("no eol after the armor header");
        errcode = RNP_ERROR_BAD_FORMAT;
        goto finish;
    }
    /* parsing headers */
    if (!armor_parse_headers(param)) {
        RNP_LOG("failed to parse headers");
        errcode = RNP_ERROR_BAD_FORMAT;
        goto finish;
    }

    /* now we are good to go with base64-encoded data */
    errcode = RNP_SUCCESS;
finish:
    if (errcode) {
        src->close();
    }
    return errcode;
}

/** @brief Write message header to the dst. */
static void
armor_write_message_header(pgp_dest_armored_param_t *param, bool finish)
{
    const char *str = finish ? ST_ARMOR_END : ST_ARMOR_BEGIN;
    dst_write(param->writedst, str, strlen(str));
    switch (param->type) {
    case PGP_ARMORED_PUBLIC_KEY:
        str = "PUBLIC KEY BLOCK";
        break;
    case PGP_ARMORED_SECRET_KEY:
        str = "PRIVATE KEY BLOCK";
        break;
    case PGP_ARMORED_SIGNATURE:
        str = "SIGNATURE";
        break;
    case PGP_ARMORED_MESSAGE:
    default:
        str = "MESSAGE";
        break;
    }
    dst_write(param->writedst, str, strlen(str));
    dst_write(param->writedst, ST_DASHES, strlen(ST_DASHES));
}

static void
armor_write_eol(pgp_dest_armored_param_t *param)
{
    if (param->eol[0]) {
        dst_write(param->writedst, &param->eol[0], 1);
    }
    if (param->eol[1]) {
        dst_write(param->writedst, &param->eol[1], 1);
    }
}

static void
armor_append_eol(pgp_dest_armored_param_t *param, uint8_t *&ptr)
{
    if (param->eol[0]) {
        *ptr++ = param->eol[0];
    }
    if (param->eol[1]) {
        *ptr++ = param->eol[1];
    }
}

/* Base 64 encoded table, quadruplicated to save cycles on use & 0x3f operation  */
static const uint8_t B64ENC[256] = {
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R',
  'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j',
  'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1',
  '2', '3', '4', '5', '6', '7', '8', '9', '+', '/', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
  'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
  'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
  's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
  '+', '/', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
  'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h',
  'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
  '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/', 'A', 'B', 'C', 'D', 'E', 'F',
  'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
  'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p',
  'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7',
  '8', '9', '+', '/'};

static void
armored_encode3(uint8_t *out, uint8_t *in)
{
    out[0] = B64ENC[in[0] >> 2];
    out[1] = B64ENC[((in[0] << 4) | (in[1] >> 4)) & 0xff];
    out[2] = B64ENC[((in[1] << 2) | (in[2] >> 6)) & 0xff];
    out[3] = B64ENC[in[2] & 0xff];
}

static rnp_result_t
armored_dst_write(pgp_dest_t *dst, const void *buf, size_t len)
{
    pgp_dest_armored_param_t *param = (pgp_dest_armored_param_t *) dst->param;
    if (!param) {
        RNP_LOG("wrong param");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* update crc */
    bool base64 = param->type == PGP_ARMORED_BASE64;
    if (!base64) {
        try {
            param->crc_ctx->add(buf, len);
        } catch (const std::exception &e) {
            RNP_LOG("%s", e.what());
            return RNP_ERROR_BAD_STATE;
        }
    }

    uint8_t  encbuf[PGP_INPUT_CACHE_SIZE / 2];
    uint8_t *bufptr = (uint8_t *) buf;
    uint8_t *bufend = bufptr + len;
    uint8_t *encptr = encbuf;
    /* processing tail if any */
    if (len + param->tailc < 3) {
        memcpy(&param->tail[param->tailc], buf, len);
        param->tailc += len;
        return RNP_SUCCESS;
    } else if (param->tailc > 0) {
        uint8_t dec3[3] = {0};
        memcpy(dec3, param->tail, param->tailc);
        memcpy(&dec3[param->tailc], bufptr, 3 - param->tailc);
        bufptr += 3 - param->tailc;
        param->tailc = 0;
        armored_encode3(encptr, dec3);
        encptr += 4;
        param->lout += 4;
        if (param->lout == param->llen) {
            armor_append_eol(param, encptr);
            param->lout = 0;
        }
    }

    /* this version prints whole chunks, so rounding down to the closest 4 */
    auto adjusted_llen = param->llen & ~3;
    /* number of input bytes to form a whole line of output, param->llen / 4 * 3 */
    auto inllen = (adjusted_llen >> 2) + (adjusted_llen >> 1);
    /* pointer to the last full line space in encbuf */
    auto enclast = encbuf + sizeof(encbuf) - adjusted_llen - 2;

    /* processing line chunks, this is the main performance-hitting cycle */
    while (bufptr + 3 <= bufend) {
        /* checking whether we have enough space in encbuf */
        if (encptr > enclast) {
            dst_write(param->writedst, encbuf, encptr - encbuf);
            encptr = encbuf;
        }
        /* setup length of the input to process in this iteration */
        uint8_t *inlend =
          !param->lout ? bufptr + inllen : bufptr + ((adjusted_llen - param->lout) >> 2) * 3;
        if (inlend > bufend) {
            /* no enough input for the full line */
            inlend = bufptr + (bufend - bufptr) / 3 * 3;
            param->lout += (inlend - bufptr) / 3 * 4;
        } else {
            /* we have full line of input */
            param->lout = 0;
        }

        /* processing one line */
        while (bufptr < inlend) {
            uint32_t t = (bufptr[0] << 16) | (bufptr[1] << 8) | (bufptr[2]);
            bufptr += 3;
            *encptr++ = B64ENC[(t >> 18) & 0xff];
            *encptr++ = B64ENC[(t >> 12) & 0xff];
            *encptr++ = B64ENC[(t >> 6) & 0xff];
            *encptr++ = B64ENC[t & 0xff];
        }

        /* adding line ending */
        if (!param->lout) {
            armor_append_eol(param, encptr);
        }
    }

    dst_write(param->writedst, encbuf, encptr - encbuf);

    /* saving tail */
    param->tailc = bufend - bufptr;
    memcpy(param->tail, bufptr, param->tailc);

    return RNP_SUCCESS;
}

static rnp_result_t
armored_dst_finish(pgp_dest_t *dst)
{
    pgp_dest_armored_param_t *param = (pgp_dest_armored_param_t *) dst->param;

    /* writing tail */
    uint8_t buf[5];
    if (param->tailc == 1) {
        buf[0] = B64ENC[param->tail[0] >> 2];
        buf[1] = B64ENC[(param->tail[0] << 4) & 0xff];
        buf[2] = CH_EQ;
        buf[3] = CH_EQ;
        dst_write(param->writedst, buf, 4);
    } else if (param->tailc == 2) {
        buf[0] = B64ENC[(param->tail[0] >> 2)];
        buf[1] = B64ENC[((param->tail[0] << 4) | (param->tail[1] >> 4)) & 0xff];
        buf[2] = B64ENC[(param->tail[1] << 2) & 0xff];
        buf[3] = CH_EQ;
        dst_write(param->writedst, buf, 4);
    }
    /* Check for base64 */
    if (param->type == PGP_ARMORED_BASE64) {
        return param->writedst->werr;
    }

    /* writing EOL if needed */
    if ((param->tailc > 0) || (param->lout > 0)) {
        armor_write_eol(param);
    }

    /* writing CRC and EOL */
    // At this point crc_ctx is initialized, so call can't fail
    buf[0] = CH_EQ;
    try {
        auto crc = param->crc_ctx->finish();
        armored_encode3(&buf[1], crc.data());
    } catch (const std::exception &e) {
        RNP_LOG("%s", e.what());
    }
    dst_write(param->writedst, buf, 5);
    armor_write_eol(param);

    /* writing armor header */
    armor_write_message_header(param, true);
    armor_write_eol(param);
    return param->writedst->werr;
}

static void
armored_dst_close(pgp_dest_t *dst, bool discard)
{
    pgp_dest_armored_param_t *param = (pgp_dest_armored_param_t *) dst->param;

    if (!param) {
        return;
    }
    /* dst_close may be called without dst_finish on error */
    delete param;
    dst->param = NULL;
}

rnp_result_t
init_armored_dst(pgp_dest_t *dst, pgp_dest_t *writedst, pgp_armored_msg_t msgtype)
{
    if (!init_dst_common(dst, 0)) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    pgp_dest_armored_param_t *param = new (std::nothrow) pgp_dest_armored_param_t();
    if (!param) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    dst->param = param;
    dst->write = armored_dst_write;
    dst->finish = armored_dst_finish;
    dst->close = armored_dst_close;
    dst->type = PGP_STREAM_ARMORED;
    dst->writeb = 0;
    dst->clen = 0;

    param->writedst = writedst;
    param->type = msgtype;
    /* Base64 message */
    if (msgtype == PGP_ARMORED_BASE64) {
        /* Base64 encoding will not output EOLs but we need this to not duplicate code for a
         * separate base64_dst_write function */
        param->eol[0] = 0;
        param->eol[1] = 0;
        param->llen = 256;
        return RNP_SUCCESS;
    }
    /* create crc context */
    param->crc_ctx = rnp::CRC24::create();
    param->eol[0] = CH_CR;
    param->eol[1] = CH_LF;
    param->llen = 76; /* must be multiple of 4 */
    /* armor header */
    armor_write_message_header(param, false);
    armor_write_eol(param);
    /* empty line */
    armor_write_eol(param);
    return RNP_SUCCESS;
}

bool
is_armored_dest(pgp_dest_t *dst)
{
    return dst->type == PGP_STREAM_ARMORED;
}

rnp_result_t
armored_dst_set_line_length(pgp_dest_t *dst, size_t llen)
{
    if (!dst || (llen < ARMORED_MIN_LINE_LENGTH) || (llen > ARMORED_MAX_LINE_LENGTH) ||
        !dst->param || !is_armored_dest(dst)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto param = (pgp_dest_armored_param_t *) dst->param;
    param->llen = llen;
    return RNP_SUCCESS;
}

bool
pgp_source_t::is_armored()
{
    char   buf[ARMORED_PEEK_BUF_SIZE] = {0};
    size_t read = 0;

    if (!peek(buf, sizeof(buf) - 1, &read) || (read < strlen(ST_ARMOR_BEGIN) + 1)) {
        return false;
    }
    buf[read] = 0;
    if (!!strstr(buf, ST_CLEAR_BEGIN)) {
        return false;
    }
    return !!strstr(buf, ST_ARMOR_BEGIN);
}

bool
pgp_source_t::is_cleartext()
{
    char   buf[ARMORED_PEEK_BUF_SIZE] = {0};
    size_t read = 0;

    if (!peek(buf, sizeof(buf) - 1, &read) || (read < strlen(ST_CLEAR_BEGIN))) {
        return false;
    }
    buf[read] = 0;
    return !!strstr(buf, ST_CLEAR_BEGIN);
}

bool
pgp_source_t::is_base64()
{
    char   buf[128] = {0};
    size_t read = 0;

    if (!peek(buf, sizeof(buf), &read) || (read < 4)) {
        return false;
    }
    return is_base64_line(buf, read);
}

rnp_result_t
rnp_dearmor_source(pgp_source_t *src, pgp_dest_t *dst)
{
    rnp_result_t res = RNP_ERROR_BAD_FORMAT;
    pgp_source_t armorsrc = {0};

    /* initializing armored message */
    res = init_armored_src(&armorsrc, src);
    if (res) {
        return res;
    }
    /* Reading data from armored source and writing it to the output */
    res = dst_write_src(&armorsrc, dst);
    if (res) {
        RNP_LOG("dearmoring failed");
    }

    armorsrc.close();
    return res;
}

rnp_result_t
rnp_armor_source(pgp_source_t *src, pgp_dest_t *dst, pgp_armored_msg_t msgtype)
{
    pgp_dest_t   armordst = {0};
    rnp_result_t res = init_armored_dst(&armordst, dst, msgtype);
    if (res) {
        return res;
    }

    res = dst_write_src(src, &armordst);
    if (res) {
        RNP_LOG("armoring failed");
    }

    dst_close(&armordst, res != RNP_SUCCESS);
    return res;
}

namespace rnp {

const uint32_t ArmoredSource::AllowBinary = 0x01;
const uint32_t ArmoredSource::AllowBase64 = 0x02;
const uint32_t ArmoredSource::AllowMultiple = 0x04;

ArmoredSource::ArmoredSource(pgp_source_t &readsrc, uint32_t flags)
    : Source(), readsrc_(readsrc), multiple_(false)
{
    /* Do not dearmor already armored stream */
    bool already = readsrc_.type == PGP_STREAM_ARMORED;
    /* Check for base64 source: no multiple streams allowed */
    if (!already && (flags & AllowBase64) && (readsrc.is_base64())) {
        auto res = init_armored_src(&src_, &readsrc_, true);
        if (res) {
            RNP_LOG("Failed to parse base64 data.");
            throw rnp::rnp_exception(res);
        }
        armored_ = true;
        return;
    }
    /* Check for armored source */
    if (!already && readsrc.is_armored()) {
        auto res = init_armored_src(&src_, &readsrc_);
        if (res) {
            RNP_LOG("Failed to parse armored data.");
            throw rnp::rnp_exception(res);
        }
        armored_ = true;
        multiple_ = flags & AllowMultiple;
        return;
    }
    /* Use binary source if allowed */
    if (!(flags & AllowBinary)) {
        RNP_LOG("Non-armored data is not allowed here.");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
    armored_ = false;
}

void
ArmoredSource::restart()
{
    if (!armored_ || readsrc_.eof() || readsrc_.error()) {
        return;
    }
    src_.close();
    auto res = init_armored_src(&src_, &readsrc_);
    if (res) {
        throw rnp::rnp_exception(res);
    }
}

pgp_source_t &
ArmoredSource::src()
{
    return armored_ ? src_ : readsrc_;
}
} // namespace rnp
