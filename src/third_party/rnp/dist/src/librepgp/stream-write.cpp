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
#include "repgp/repgp_def.h"
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <cassert>
#ifdef HAVE_UNISTD_H
#include <sys/param.h>
#include <unistd.h>
#else
#include "uniwin.h"
#endif
#include <string.h>
#ifdef HAVE_ZLIB_H
#include <zlib.h>
#endif
#ifdef HAVE_BZLIB_H
#include <bzlib.h>
#endif
#include <rnp/rnp_def.h>
#include "stream-def.h"
#include "stream-ctx.h"
#include "stream-write.h"
#include "stream-packet.h"
#include "stream-armor.h"
#include "stream-sig.h"
#include "key.hpp"
#include "fingerprint.hpp"
#include "types.h"
#include "crypto/signatures.h"
#include "defaults.h"
#include <time.h>
#include <algorithm>
#ifdef ENABLE_CRYPTO_REFRESH
#include "v2_seipd.h"
#endif

/* 8192 bytes, as GnuPG */
#define PGP_PARTIAL_PKT_SIZE_BITS (13)
#define PGP_PARTIAL_PKT_BLOCK_SIZE (1 << PGP_PARTIAL_PKT_SIZE_BITS)

/* common fields for encrypted, compressed and literal data */
typedef struct pgp_dest_packet_param_t {
    pgp_dest_t *writedst;                 /* destination to write to, could be partial */
    pgp_dest_t *origdst;                  /* original dest passed to init_*_dst */
    bool        partial;                  /* partial length packet */
    bool        indeterminate;            /* indeterminate length packet */
    int         tag;                      /* packet tag */
    uint8_t     hdr[PGP_MAX_HEADER_SIZE]; /* header, including length, as it was written */
    size_t      hdrlen;                   /* number of bytes in hdr */
} pgp_dest_packet_param_t;

typedef struct pgp_dest_compressed_param_t {
    pgp_dest_packet_param_t pkt;
    pgp_compression_type_t  alg;
    union {
        z_stream  z;
        bz_stream bz;
    };
    bool    zstarted;                        /* whether we initialize zlib/bzip2  */
    uint8_t cache[PGP_INPUT_CACHE_SIZE / 2]; /* pre-allocated cache for compression */
    size_t  len;                             /* number of bytes cached */
} pgp_dest_compressed_param_t;

typedef struct pgp_dest_encrypted_param_t {
    pgp_dest_packet_param_t    pkt{}; /* underlying packet-related params */
    rnp_ctx_t &                ctx;   /* rnp operation context with additional parameters */
    rnp::AuthType              auth_type{}; /* Authentication type: MDC, AEAD or none */
    pgp_crypt_t                encrypt{};   /* encrypting crypto */
    std::unique_ptr<rnp::Hash> mdc;         /* mdc SHA1 hash */
    pgp_aead_alg_t             aalg;        /* AEAD algorithm used */
    uint8_t                    iv[PGP_AEAD_MAX_NONCE_LEN]{}; /* iv for AEAD mode */
    uint8_t                    ad[PGP_AEAD_MAX_AD_LEN]{}; /* additional data for AEAD mode */
    size_t                     adlen{};    /* length of additional data, including chunk idx */
    size_t                     chunklen{}; /* length of the AEAD chunk in bytes */
    size_t                     chunkout{}; /* how many bytes from the chunk were written out */
    size_t                     chunkidx{}; /* index of the current AEAD chunk */
    size_t                     cachelen{}; /* how many bytes are in cache, for AEAD */
    uint8_t cache[PGP_AEAD_CACHE_LEN]{};   /* pre-allocated cache for encryption */
#ifdef ENABLE_CRYPTO_REFRESH
    std::array<uint8_t, PGP_SEIPDV2_SALT_LEN> v2_seipd_salt; /* SEIPDv2 salt value */
#endif
    std::list<pgp_sk_sesskey_t> skesks;

    pgp_dest_encrypted_param_t(rnp_ctx_t &actx) : ctx(actx), aalg(PGP_AEAD_NONE)
    {
    }

    bool
    is_aead_auth()
    {
        switch (this->auth_type) {
        case rnp::AuthType::AEADv1:
#ifdef ENABLE_CRYPTO_REFRESH
        case rnp::AuthType::AEADv2:
#endif
            return true;
        default:
            return false;
        }
    }

#ifdef ENABLE_CRYPTO_REFRESH
    bool
    is_v2_seipd() const
    {
        return this->auth_type == rnp::AuthType::AEADv2;
    }
#endif
} pgp_dest_encrypted_param_t;

typedef struct pgp_dest_signer_info_t {
    pgp_one_pass_sig_t onepass;
    rnp::Key *         key;
    pgp_hash_alg_t     halg;
    int64_t            sigcreate;
    uint64_t           sigexpire;
} pgp_dest_signer_info_t;

typedef struct pgp_dest_signed_param_t {
    pgp_dest_t *             writedst; /* destination to write to */
    rnp_ctx_t *              ctx;      /* rnp operation context with additional parameters */
    pgp_password_provider_t *password_provider;   /* password provider from write handler */
    std::vector<pgp_dest_signer_info_t> siginfos; /* list of  pgp_dest_signer_info_t */
    rnp::HashList hashes;              /* hashes to pass raw data through and then sign */
    bool          clr_start;           /* we are on the start of the line */
    uint8_t       clr_buf[CT_BUF_LEN]; /* buffer to hold partial line data */
    size_t        clr_buflen;          /* number of bytes in buffer */

    pgp_literal_hdr_t lhdr{}; /* literal packet header, needed for v5 sigs */
    bool              has_lhdr = false;

    pgp_dest_signed_param_t() = default;
    ~pgp_dest_signed_param_t() = default;
} pgp_dest_signed_param_t;

typedef struct pgp_dest_partial_param_t {
    pgp_dest_t *writedst;
    uint8_t     part[PGP_PARTIAL_PKT_BLOCK_SIZE];
    uint8_t     parthdr; /* header byte for the current part */
    size_t      partlen; /* length of the current part, up to PARTIAL_PKT_BLOCK_SIZE */
    size_t      len;     /* bytes cached in part */
} pgp_dest_partial_param_t;

static rnp_result_t
partial_dst_write(pgp_dest_t *dst, const void *buf, size_t len)
{
    pgp_dest_partial_param_t *param = (pgp_dest_partial_param_t *) dst->param;
    if (!param) {
        /* LCOV_EXCL_START */
        RNP_LOG("wrong param");
        return RNP_ERROR_BAD_PARAMETERS;
        /* LCOV_EXCL_END */
    }

    if (len > param->partlen - param->len) {
        /* we have full part - in block and in buf */
        size_t wrlen = param->partlen - param->len;
        dst_write(param->writedst, &param->parthdr, 1);
        dst_write(param->writedst, param->part, param->len);
        dst_write(param->writedst, buf, wrlen);

        buf = (uint8_t *) buf + wrlen;
        len -= wrlen;
        param->len = 0;

        /* writing all full parts directly from buf */
        while (len >= param->partlen) {
            dst_write(param->writedst, &param->parthdr, 1);
            dst_write(param->writedst, buf, param->partlen);
            buf = (uint8_t *) buf + param->partlen;
            len -= param->partlen;
        }
    }

    /* caching rest of the buf */
    if (len > 0) {
        memcpy(&param->part[param->len], buf, len);
        param->len += len;
    }

    return RNP_SUCCESS;
}

static rnp_result_t
partial_dst_finish(pgp_dest_t *dst)
{
    pgp_dest_partial_param_t *param = (pgp_dest_partial_param_t *) dst->param;
    uint8_t                   hdr[5];
    int                       lenlen;

    lenlen = write_packet_len(hdr, param->len);
    dst_write(param->writedst, hdr, lenlen);
    dst_write(param->writedst, param->part, param->len);

    return param->writedst->werr;
}

static void
partial_dst_close(pgp_dest_t *dst, bool discard)
{
    pgp_dest_partial_param_t *param = (pgp_dest_partial_param_t *) dst->param;
    free(param);
    dst->param = NULL;
}

static rnp_result_t
init_partial_pkt_dst(pgp_dest_t &dst, pgp_dest_t &writedst)
{
    pgp_dest_partial_param_t *param = NULL;

    if (!init_dst_common(&dst, sizeof(*param))) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }

    param = (pgp_dest_partial_param_t *) dst.param;
    param->writedst = &writedst;
    param->partlen = PGP_PARTIAL_PKT_BLOCK_SIZE;
    param->parthdr = 0xE0 | PGP_PARTIAL_PKT_SIZE_BITS;
    dst.param = param;
    dst.write = partial_dst_write;
    dst.finish = partial_dst_finish;
    dst.close = partial_dst_close;
    dst.type = PGP_STREAM_PARLEN_PACKET;

    return RNP_SUCCESS;
}

/** @brief helper function for streamed packets (literal, encrypted and compressed).
 *  Allocates part len destination if needed and writes header
 **/
static bool
init_streamed_packet(pgp_dest_packet_param_t &param, pgp_dest_t &dst)
{
    rnp_result_t ret = RNP_ERROR_GENERIC;

    if (param.partial) {
        param.hdr[0] = param.tag | PGP_PTAG_ALWAYS_SET | PGP_PTAG_NEW_FORMAT;
        dst_write(&dst, &param.hdr, 1);

        if (!(param.writedst = (pgp_dest_t *) calloc(1, sizeof(*param.writedst)))) {
            RNP_LOG("part len dest allocation failed");
            return false;
        }
        ret = init_partial_pkt_dst(*param.writedst, dst);
        if (ret) {
            /* LCOV_EXCL_START */
            free(param.writedst);
            param.writedst = NULL;
            return false;
            /* LCOV_EXCL_END */
        }
        param.origdst = &dst;

        param.hdr[1] = ((pgp_dest_partial_param_t *) param.writedst->param)->parthdr;
        param.hdrlen = 2;
        return true;
    }

    /* LCOV_EXCL_START this branch is not used at all */
    if (param.indeterminate) {
        if (param.tag > 0xf) {
            RNP_LOG("indeterminate tag > 0xf");
        }

        param.hdr[0] = ((param.tag & 0xf) << PGP_PTAG_OF_CONTENT_TAG_SHIFT) |
                       PGP_PTAG_OLD_LEN_INDETERMINATE;
        param.hdrlen = 1;
        dst_write(&dst, &param.hdr, 1);

        param.writedst = &dst;
        param.origdst = &dst;
        return true;
    }

    RNP_LOG("wrong call");
    return false;
    /* LCOV_EXCL_END */
}

static rnp_result_t
finish_streamed_packet(pgp_dest_packet_param_t *param)
{
    if (param->partial) {
        return dst_finish(param->writedst);
    }
    return RNP_SUCCESS;
}

static void
close_streamed_packet(pgp_dest_packet_param_t *param, bool discard)
{
    if (param->partial) {
        dst_close(param->writedst, discard);
        free(param->writedst);
        param->writedst = NULL;
    }
}

static rnp_result_t
encrypted_dst_write_cfb(pgp_dest_t *dst, const void *buf, size_t len)
{
    auto param = static_cast<pgp_dest_encrypted_param_t *>(dst->param);
    if (!param) {
        /* LCOV_EXCL_START */
        RNP_LOG("wrong param");
        return RNP_ERROR_BAD_PARAMETERS;
        /* LCOV_EXCL_END */
    }

    if (param->auth_type == rnp::AuthType::MDC) {
        try {
            param->mdc->add(buf, len);
        } catch (const std::exception &e) {
            /* LCOV_EXCL_START */
            RNP_LOG("%s", e.what());
            return RNP_ERROR_BAD_STATE;
            /* LCOV_EXCL_END */
        }
    }

    while (len > 0) {
        size_t sz = len > sizeof(param->cache) ? sizeof(param->cache) : len;
        pgp_cipher_cfb_encrypt(&param->encrypt, param->cache, (const uint8_t *) buf, sz);
        dst_write(param->pkt.writedst, param->cache, sz);
        len -= sz;
        buf = (uint8_t *) buf + sz;
    }

    return RNP_SUCCESS;
}

#if defined(ENABLE_AEAD)
static rnp_result_t
encrypted_start_aead_chunk(pgp_dest_encrypted_param_t *param, size_t idx, bool last)
{
    uint8_t  nonce[PGP_AEAD_MAX_NONCE_LEN];
    size_t   nlen;
    size_t   taglen;
    bool     res;
    uint64_t total;

    taglen = pgp_cipher_aead_tag_len(param->aalg);

    /* finish the previous chunk if needed*/
    if ((idx > 0) && (param->chunkout + param->cachelen > 0)) {
        if (param->cachelen + taglen > sizeof(param->cache)) {
            RNP_LOG("wrong state in aead");
            return RNP_ERROR_BAD_STATE;
        }

        if (!pgp_cipher_aead_finish(
              &param->encrypt, param->cache, param->cache, param->cachelen)) {
            return RNP_ERROR_BAD_STATE;
        }

        dst_write(param->pkt.writedst, param->cache, param->cachelen + taglen);
    }

    /* set chunk index for additional data */
    if (param->auth_type == rnp::AuthType::AEADv1) {
        write_uint64(param->ad + param->adlen - 8, idx);
    }

    if (last) {
        if (!(param->chunkout + param->cachelen)) {
            /* we need to clearly reset it since cipher was initialized but not finished */
            pgp_cipher_aead_reset(&param->encrypt);
        }

        total = idx * param->chunklen;
        if (param->cachelen + param->chunkout) {
            if (param->chunklen < (param->cachelen + param->chunkout)) {
                RNP_LOG("wrong last chunk state in aead");
                return RNP_ERROR_BAD_STATE;
            }
            total -= param->chunklen - param->cachelen - param->chunkout;
        }

        write_uint64(param->ad + param->adlen, total);
        param->adlen += 8;
    }
    if (!pgp_cipher_aead_set_ad(&param->encrypt, param->ad, param->adlen)) {
        RNP_LOG("failed to set ad");
        return RNP_ERROR_BAD_STATE;
    }

    /* set chunk index for nonce */
    nlen = pgp_cipher_aead_nonce(param->aalg, param->iv, nonce, idx);
    if (!nlen) {
        RNP_LOG(
          "ERROR: when starting encrypted AEAD chunk: could not determine nonce length"); // LCOV_EXCL_LINE
    }

    /* start cipher */
    res = pgp_cipher_aead_start(&param->encrypt, nonce, nlen);

    /* write final authentication tag */
    if (last) {
        res = res && pgp_cipher_aead_finish(&param->encrypt, param->cache, param->cache, 0);
        if (res) {
            dst_write(param->pkt.writedst, param->cache, taglen);
        }
    }

    param->chunkidx = idx;
    param->chunkout = 0;

    return res ? RNP_SUCCESS : RNP_ERROR_BAD_PARAMETERS;
}
#endif

static rnp_result_t
encrypted_dst_write_aead(pgp_dest_t *dst, const void *buf, size_t len)
{
#if !defined(ENABLE_AEAD)
    RNP_LOG("AEAD is not enabled.");
    return RNP_ERROR_WRITE;
#else
    auto param = static_cast<pgp_dest_encrypted_param_t *>(dst->param);

    size_t       sz;
    size_t       gran;
    rnp_result_t res;

    if (!param) {
        /* LCOV_EXCL_START */
        RNP_LOG("wrong param");
        return RNP_ERROR_BAD_PARAMETERS;
        /* LCOV_EXCL_END */
    }

    if (!len) {
        return RNP_SUCCESS;
    }

    /* because of botan's FFI granularity we need to make things a bit complicated */
    gran = pgp_cipher_aead_granularity(&param->encrypt);

    if (param->cachelen > param->chunklen - param->chunkout) {
        RNP_LOG("wrong AEAD cache state");
        return RNP_ERROR_BAD_STATE;
    }

    while (len > 0) {
        /* 2 tags to align to the PGP_INPUT_CACHE_SIZE size */
        sz = std::min(sizeof(param->cache) - 2 * PGP_AEAD_MAX_TAG_LEN - param->cachelen, len);
        sz = std::min(sz, param->chunklen - param->chunkout - param->cachelen);
        memcpy(param->cache + param->cachelen, buf, sz);
        param->cachelen += sz;

        if (param->cachelen == param->chunklen - param->chunkout) {
            /* we have the tail of the chunk in cache */
            if ((res = encrypted_start_aead_chunk(param, param->chunkidx + 1, false))) {
                return res;
            }
            param->cachelen = 0;
        } else if (param->cachelen >= gran) {
            /* we have part of the chunk - so need to adjust it to the granularity */
            size_t gransz = param->cachelen - param->cachelen % gran;
            size_t inread = 0;
            if (!pgp_cipher_aead_update(
                  param->encrypt, param->cache, param->cache, gransz, inread)) {
                return RNP_ERROR_BAD_STATE;
            }
            if (inread != gransz) {
                /* LCOV_EXCL_START */
                RNP_LOG("Unexpected aead update: read %zu instead of %zu.", inread, gransz);
                return RNP_ERROR_BAD_STATE;
                /* LCOV_EXCL_END */
            }
            dst_write(param->pkt.writedst, param->cache, gransz);
            memmove(param->cache, param->cache + gransz, param->cachelen - gransz);
            param->cachelen -= gransz;
            param->chunkout += gransz;
        }

        len -= sz;
        buf = (uint8_t *) buf + sz;
    }

    return RNP_SUCCESS;
#endif
}

static rnp_result_t
encrypted_dst_finish(pgp_dest_t *dst)
{
    auto param = static_cast<pgp_dest_encrypted_param_t *>(dst->param);

    if (param->is_aead_auth()) {
#if !defined(ENABLE_AEAD)
        RNP_LOG("AEAD is not enabled.");
        rnp_result_t res = RNP_ERROR_NOT_IMPLEMENTED;
#else
        size_t chunks = param->chunkidx;
        /* if we didn't write anything in current chunk then discard it and restart */
        if (param->chunkout || param->cachelen) {
            chunks++;
        }

        rnp_result_t res = encrypted_start_aead_chunk(param, chunks, true);
        pgp_cipher_aead_destroy(&param->encrypt);
#endif
        if (res) {
            finish_streamed_packet(&param->pkt);
            return res;
        }
    } else if (param->auth_type == rnp::AuthType::MDC) {
        uint8_t mdcbuf[MDC_V1_SIZE];
        mdcbuf[0] = MDC_PKT_TAG;
        mdcbuf[1] = MDC_V1_SIZE - 2;
        try {
            param->mdc->add(mdcbuf, 2);
            param->mdc->finish(&mdcbuf[2]);
            param->mdc = nullptr;
        } catch (const std::exception &e) {
            /* LCOV_EXCL_START */
            RNP_LOG("%s", e.what());
            return RNP_ERROR_BAD_STATE;
            /* LCOV_EXCL_END */
        }
        pgp_cipher_cfb_encrypt(&param->encrypt, mdcbuf, mdcbuf, MDC_V1_SIZE);
        dst_write(param->pkt.writedst, mdcbuf, MDC_V1_SIZE);
    }

    return finish_streamed_packet(&param->pkt);
}

static void
encrypted_dst_close(pgp_dest_t *dst, bool discard)
{
    auto param = static_cast<pgp_dest_encrypted_param_t *>(dst->param);
    if (!param) {
        return; // LCOV_EXCL_LINE
    }

    if (param->is_aead_auth()) {
#if defined(ENABLE_AEAD)
        pgp_cipher_aead_destroy(&param->encrypt);
#endif
    } else {
        pgp_cipher_cfb_finish(&param->encrypt);
    }
    close_streamed_packet(&param->pkt, discard);
    delete param;
    dst->param = NULL;
}

static rnp_result_t
encrypted_add_recipient(rnp_ctx_t &              ctx,
                        pgp_dest_t &             dst,
                        rnp::Key *               userkey,
                        const rnp::secure_bytes &key,
                        pgp_pkesk_version_t      pkesk_version)
{
    auto param = static_cast<pgp_dest_encrypted_param_t *>(dst.param);

    /* Use primary key if good for encryption, otherwise look in subkey list */
    userkey = find_suitable_key(PGP_OP_ENCRYPT, userkey, &ctx.key_provider);
    if (!userkey) {
        return RNP_ERROR_NO_SUITABLE_KEY;
    }

#if defined(ENABLE_CRYPTO_REFRESH) || defined(ENABLE_PQC)
    /* Crypto Refresh: For X25519/X448 PKESKv3, AES is mandated */
    /* PQC: AES is mandated for PKESKv3 */
    if (!check_enforce_aes_v3_pkesk(userkey->alg(), param->ctx.ealg, pkesk_version)) {
        RNP_LOG("attempting to use v3 PKESK with an unencrypted algorithm id in "
                "combination with a symmetric "
                "algorithm that is not AES.");
        return RNP_ERROR_ENCRYPT_FAILED;
    }
#endif

    /* Fill pkey */
    pgp_pk_sesskey_t pkey;
    pkey.version = pkesk_version;
    pkey.alg = userkey->alg();
    pkey.salg = param->ctx.ealg;
    /* set key_id (used for PKESK v3) and fingerprint (used for PKESK v6) */
    pkey.key_id = userkey->keyid();
#if defined(ENABLE_CRYPTO_REFRESH)
    if (pkey.version == (uint8_t) PGP_V6) {
        pkey.fp = userkey->fp();
    }
#endif

    /* Encrypt the session key */
    rnp::secure_bytes enckey;
#if defined(ENABLE_CRYPTO_REFRESH) || defined(ENABLE_PQC)
    if ((pkey.version == PGP_PKSK_V3) && do_encrypt_pkesk_v3_alg_id(pkey.alg)) {
        /* for pre-crypto-refresh algorithms, algorithm ID is part of the session key */
        enckey.push_back(pkey.salg);
    }
#else
    enckey.push_back(pkey.salg);
#endif
    enckey.insert(enckey.end(), key.begin(), key.end());

    if (have_pkesk_checksum(pkey.alg)) {
        /* Calculate checksum */
        rnp::secure_array<uint16_t, 1> checksum;
        for (size_t i = 0; i < key.size(); i++) {
            checksum[0] += key[i];
        }
        enckey.push_back(checksum[0] >> 8);
        enckey.push_back(checksum[0] & 0xff);
    }

#if defined(ENABLE_PQC_DBG_LOG)
    RNP_LOG_U8VEC("Session Key: %s", std::vector<uint8_t>(enckey.begin(), enckey.end()));
#endif

    auto material = pgp::EncMaterial::create(pkey.alg);
#if defined(ENABLE_CRYPTO_REFRESH)
    material->version = pkey.version;
    material->salg = pkey.salg;
#endif
    if (userkey->alg() == PGP_PKA_ECDH) {
        auto ecdh = dynamic_cast<pgp::ECDHEncMaterial *>(material.get());
        assert(ecdh);
        ecdh->enc.fp = userkey->fp().vec();
    }
    auto ret = userkey->pkt().material->encrypt(ctx.sec_ctx, *material, enckey);
    if (ret) {
        return ret;
    }

    /* Writing symmetric key encrypted session key packet */
    try {
        pkey.write_material(*material);
        pkey.write(*param->pkt.origdst);
        return param->pkt.origdst->werr;
    } catch (const std::exception &e) {
        return RNP_ERROR_WRITE; // LCOV_EXCL_LINE
    }
}

static void
encrypted_add_password_v4_single(pgp_dest_encrypted_param_t &param,
                                 rnp::secure_bytes &         key,
                                 size_t                      keylen)
{
    assert(param.ctx.passwords.size() == 1);
    pgp_sk_sesskey_t skey{};
    auto &           pass = param.ctx.passwords.front();
    skey.version = PGP_SKSK_V4;
    skey.s2k = pass.s2k;
    /* if there are no public keys then we do not encrypt session key in the packet */
    skey.alg = param.ctx.ealg;
    skey.enckeylen = 0;
    key.assign(pass.key.data(), pass.key.data() + keylen);
    param.skesks.push_back(std::move(skey));
}

static bool
encrypted_add_password_v4(pgp_dest_encrypted_param_t &     param,
                          const rnp_symmetric_pass_info_t &pass,
                          const rnp::secure_bytes &        key)
{
    pgp_sk_sesskey_t skey{};
    skey.version = PGP_SKSK_V4;
    skey.s2k = pass.s2k;
    /* We may use different algo for CEK and KEK */
    skey.enckeylen = key.size() + 1;
    skey.enckey[0] = param.ctx.ealg;
    memcpy(&skey.enckey[1], key.data(), key.size());
    skey.alg = pass.s2k_cipher;
    pgp_crypt_t kcrypt;
    if (!pgp_cipher_cfb_start(&kcrypt, skey.alg, pass.key.data(), NULL)) {
        RNP_LOG("key encryption failed");
        return false;
    }
    pgp_cipher_cfb_encrypt(&kcrypt, skey.enckey, skey.enckey, skey.enckeylen);
    pgp_cipher_cfb_finish(&kcrypt);
    param.skesks.push_back(std::move(skey));
    return true;
}

static bool
encrypted_add_password_v5(pgp_dest_encrypted_param_t &     param,
                          const rnp_symmetric_pass_info_t &pass,
                          const rnp::secure_bytes &        key)
{
#if !defined(ENABLE_AEAD)
    RNP_LOG("AEAD support is not enabled.");
    return RNP_ERROR_NOT_IMPLEMENTED;
#else
    /* AEAD-encrypted v5 packet */
    if ((param.ctx.aalg != PGP_AEAD_EAX) && (param.ctx.aalg != PGP_AEAD_OCB)) {
        RNP_LOG("unsupported AEAD algorithm");
        return false;
    }

    pgp_sk_sesskey_t skey{};
    skey.version = PGP_SKSK_V5;
    skey.s2k = pass.s2k;
    skey.alg = pass.s2k_cipher;
    skey.aalg = param.ctx.aalg;
    skey.ivlen = pgp_cipher_aead_nonce_len(skey.aalg);
    skey.enckeylen = key.size() + pgp_cipher_aead_tag_len(skey.aalg);
    param.ctx.sec_ctx.rng.get(skey.iv, skey.ivlen);

    /* initialize cipher */
    pgp_crypt_t kcrypt;
    if (!pgp_cipher_aead_init(&kcrypt, skey.alg, skey.aalg, pass.key.data(), false)) {
        return false;
    }

    /* set additional data */
    if (!encrypted_sesk_set_ad(kcrypt, skey)) {
        return false; // LCOV_EXCL_LINE
    }

    /* calculate nonce */
    uint8_t nonce[PGP_AEAD_MAX_NONCE_LEN];
    size_t  nlen = pgp_cipher_aead_nonce(skey.aalg, skey.iv, nonce, 0);

    /* start cipher, encrypt key and get tag */
    bool res = pgp_cipher_aead_start(&kcrypt, nonce, nlen) &&
               pgp_cipher_aead_finish(&kcrypt, skey.enckey, key.data(), key.size());

    pgp_cipher_aead_destroy(&kcrypt);
    if (res) {
        param.skesks.push_back(std::move(skey));
    }
    return res;
#endif
}

static bool
encrypted_build_cek(pgp_dest_encrypted_param_t &param,
                    rnp::secure_bytes &         key,
                    const size_t                keylen)
{
    auto &ctx = param.ctx;
    if (ctx.passwords.empty()) {
        ctx.sec_ctx.rng.get(key.data(), keylen);
        return true;
    }

    /* v4 single-password encryption case */
    if (ctx.recipients.empty() && (ctx.passwords.size() == 1) && !param.is_aead_auth()) {
        encrypted_add_password_v4_single(param, key, keylen);
        return true;
    }

    size_t attempts = 0;
gencek:
    ctx.sec_ctx.rng.get(key.data(), keylen);
    for (auto &pass : param.ctx.passwords) {
        bool res = param.is_aead_auth() ? encrypted_add_password_v5(param, pass, key) :
                                          encrypted_add_password_v4(param, pass, key);
        if (!res) {
            return false;
        }
    }

    if (param.is_aead_auth()) {
        return true;
    }
    /* Workaround for GnuPG symmetric key validity check: if first byte of the decrypted data
     * is a valid symmetric algorithm number then it uses password provided, producing an error
     * afterwards. */
    for (auto &pass : param.ctx.passwords) {
        for (auto &skey : param.skesks) {
            /* attempt to decrypt cek */
            pgp_crypt_t crypt;
            if (!pgp_cipher_cfb_start(&crypt, skey.alg, pass.key.data(), NULL)) {
                return false;
            }
            assert(skey.enckeylen == keylen + 1);
            rnp::secure_bytes dec(skey.enckeylen, 0);
            pgp_cipher_cfb_decrypt(&crypt, dec.data(), skey.enckey, skey.enckeylen);
            pgp_cipher_cfb_finish(&crypt);

            /* using the correct key */
            if (!memcmp(dec.data() + 1, key.data(), keylen)) {
                continue;
            }

            /* check whether the first byte decrypts to valid alg name */
            if (pgp_key_size(static_cast<pgp_symm_alg_t>(dec[0]))) {
                /* Do not do too many attempts */
                attempts++;
                if (attempts >= 8) {
                    break;
                }
                /* re-generate CEK */
                param.skesks.clear();
                goto gencek;
            }
        }
    }
    return true;
}

static rnp_result_t
encrypted_start_cfb(pgp_dest_encrypted_param_t *param, uint8_t *enckey)
{
    uint8_t mdcver = 1;
    uint8_t enchdr[PGP_MAX_BLOCK_SIZE + 2]; /* encrypted header */

    if (param->auth_type == rnp::AuthType::MDC) {
        /* initializing the mdc */
        dst_write(param->pkt.writedst, &mdcver, 1);

        try {
            param->mdc = rnp::Hash::create(PGP_HASH_SHA1);
        } catch (const std::exception &e) {
            /* LCOV_EXCL_START */
            RNP_LOG("cannot create sha1 hash: %s", e.what());
            return RNP_ERROR_GENERIC;
            /* LCOV_EXCL_END */
        }
    }

    /* initializing the crypto */
    size_t blsize = pgp_block_size(param->ctx.ealg);
    if (!blsize || !pgp_cipher_cfb_start(&param->encrypt, param->ctx.ealg, enckey, NULL)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* generating and writing iv/password check bytes */
    try {
        param->ctx.sec_ctx.rng.get(enchdr, blsize);
        enchdr[blsize] = enchdr[blsize - 2];
        enchdr[blsize + 1] = enchdr[blsize - 1];

        if (param->auth_type == rnp::AuthType::MDC) {
            param->mdc->add(enchdr, blsize + 2);
        }
    } catch (const std::exception &e) {
        /* LCOV_EXCL_START */
        RNP_LOG("%s", e.what());
        return RNP_ERROR_BAD_STATE;
        /* LCOV_EXCL_END */
    }

    pgp_cipher_cfb_encrypt(&param->encrypt, enchdr, enchdr, blsize + 2);

    /* RFC 4880, 5.13: Unlike the Symmetrically Encrypted Data Packet, no special CFB
     * resynchronization is done after encrypting this prefix data. */
    if (param->auth_type == rnp::AuthType::None) {
        pgp_cipher_cfb_resync(&param->encrypt, enchdr + 2);
    }

    dst_write(param->pkt.writedst, enchdr, blsize + 2);

    return RNP_SUCCESS;
}

static rnp_result_t
encrypted_start_aead(pgp_dest_encrypted_param_t *param, uint8_t *enckey)
{
#if !defined(ENABLE_AEAD)
    RNP_LOG("AEAD support is not enabled.");
    return RNP_ERROR_NOT_IMPLEMENTED;
#else
    uint8_t hdr[4 + PGP_AEAD_MAX_NONCE_OR_SALT_LEN];
    size_t  nlen;

    if (pgp_block_size(param->ctx.ealg) != 16) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* fill header */
#ifdef ENABLE_CRYPTO_REFRESH
    hdr[0] = param->auth_type == rnp::AuthType::AEADv2 ? 2 : 1;
#else
    hdr[0] = 1;
#endif
    hdr[1] = param->ctx.ealg;
    hdr[2] = param->ctx.aalg;
    hdr[3] = param->ctx.abits;

    /* generate iv */
    nlen = pgp_cipher_aead_nonce_len(param->ctx.aalg);
    uint8_t *iv_or_salt = param->iv;
    size_t   iv_or_salt_len = nlen;
#ifdef ENABLE_CRYPTO_REFRESH
    if (param->auth_type == rnp::AuthType::AEADv2) {
        iv_or_salt = param->v2_seipd_salt.data();
        iv_or_salt_len = param->v2_seipd_salt.size();
    }
#endif
    try {
        param->ctx.sec_ctx.rng.get(iv_or_salt, iv_or_salt_len);
    } catch (const std::exception &e) {
        return RNP_ERROR_RNG; // LCOV_EXCL_LINE
    }
    memcpy(hdr + 4, iv_or_salt, iv_or_salt_len);
    /* output header */
    dst_write(param->pkt.writedst, hdr, 4 + iv_or_salt_len);

    /* initialize encryption */
    param->chunklen = 1L << (hdr[3] + 6);
    param->chunkout = 0;

    /* fill additional/authenticated data */
    uint8_t raw_packet_tag = PGP_PKT_AEAD_ENCRYPTED;
#ifdef ENABLE_CRYPTO_REFRESH
    if (param->auth_type == rnp::AuthType::AEADv2) {
        raw_packet_tag = PGP_PKT_SE_IP_DATA;
    }
#endif
    param->ad[0] = raw_packet_tag | PGP_PTAG_ALWAYS_SET | PGP_PTAG_NEW_FORMAT;
    memcpy(param->ad + 1, hdr, 4);
#ifdef ENABLE_CRYPTO_REFRESH
    if (param->auth_type == rnp::AuthType::AEADv2) {
        param->adlen = 5;
    } else {
#endif
        memset(param->ad + 5, 0, 8);
        param->adlen = 13;
#ifdef ENABLE_CRYPTO_REFRESH
    }
    seipd_v2_aead_fields_t s2_fields;
    if (param->auth_type == rnp::AuthType::AEADv2) {
        param->aalg = param->ctx.aalg;
        pgp_seipdv2_hdr_t v2_seipd_hdr;
        v2_seipd_hdr.cipher_alg = param->ctx.ealg;
        v2_seipd_hdr.aead_alg = param->ctx.aalg;
        v2_seipd_hdr.chunk_size_octet = param->ctx.abits;
        v2_seipd_hdr.version = PGP_SE_IP_DATA_V2;
        memcpy(v2_seipd_hdr.salt, iv_or_salt, PGP_SEIPDV2_SALT_LEN);
        s2_fields = seipd_v2_key_and_nonce_derivation(v2_seipd_hdr, enckey);
        enckey = s2_fields.key.data();
        if (s2_fields.nonce.size() > sizeof(param->iv)) {
            // would be better to indicate an error
            s2_fields.nonce.resize(sizeof(param->iv));
        }
        std::memcpy(param->iv, s2_fields.nonce.data(), s2_fields.nonce.size());
    }
#endif

    /* initialize cipher */
    if (!pgp_cipher_aead_init(
          &param->encrypt, param->ctx.ealg, param->ctx.aalg, enckey, false)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    return encrypted_start_aead_chunk(param, 0, false);
#endif
}

static rnp_result_t
init_encrypted_dst(rnp_ctx_t &ctx, pgp_dest_t &dst, pgp_dest_t &writedst)
{
    size_t keylen = pgp_key_size(ctx.ealg);
    if (!keylen) {
        RNP_LOG("unknown symmetric algorithm");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    if (ctx.aalg != PGP_AEAD_NONE) {
        if ((ctx.aalg != PGP_AEAD_EAX) && (ctx.aalg != PGP_AEAD_OCB)) {
            /* LCOV_EXCL_START */
            RNP_LOG("unknown AEAD algorithm: %d", (int) ctx.aalg);
            return RNP_ERROR_BAD_PARAMETERS;
            /* LCOV_EXCL_END */
        }

        if ((pgp_block_size(ctx.ealg) != 16)) {
            /* LCOV_EXCL_START */
            RNP_LOG("wrong AEAD symmetric algorithm");
            return RNP_ERROR_BAD_PARAMETERS;
            /* LCOV_EXCL_END */
        }

        if ((ctx.abits < 0) || (ctx.abits > 16)) {
            /* LCOV_EXCL_START */
            RNP_LOG("wrong AEAD chunk bits: %d", ctx.abits);
            return RNP_ERROR_BAD_PARAMETERS;
            /* LCOV_EXCL_END */
        }
    }

    if (ctx.recipients.empty() && ctx.passwords.empty()) {
        RNP_LOG("no recipients");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    if (!init_dst_common(&dst, 0)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    auto param = new (std::nothrow) pgp_dest_encrypted_param_t(ctx);
    if (!param) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    dst.param = param;
    param->auth_type = ctx.aalg == PGP_AEAD_NONE ? rnp::AuthType::MDC : rnp::AuthType::AEADv1;

#if defined(ENABLE_CRYPTO_REFRESH)
    /* in the case of PKESK (pkeycount > 0) and all keys are PKESKv6/SEIPDv2 capable, upgrade
     * to AEADv2 */
    if (ctx.enable_pkesk_v6 && ctx.pkeskv6_capable() && !ctx.recipients.empty()) {
        param->auth_type = rnp::AuthType::AEADv2;
    }
#endif
    param->aalg = ctx.aalg;
    param->pkt.origdst = &writedst;
    // the following assignment is covered for the v2 SEIPD case further below
    dst.write = param->is_aead_auth() ? encrypted_dst_write_aead : encrypted_dst_write_cfb;
    dst.finish = encrypted_dst_finish;
    dst.close = encrypted_dst_close;
    dst.type = PGP_STREAM_ENCRYPTED;

    rnp::secure_bytes enckey(keylen, 0); /* content encryption key */

    /* Build SKESK and generate CEK */
    rnp_result_t ret = RNP_ERROR_BAD_PARAMETERS;
    if (!encrypted_build_cek(*param, enckey, keylen)) {
        goto finish;
    }

    /* Configuring and writing pk-encrypted session keys */
    for (auto recipient : ctx.recipients) {
        pgp_pkesk_version_t pkesk_version = PGP_PKSK_V3;
#if defined(ENABLE_CRYPTO_REFRESH)
        if (param->auth_type == rnp::AuthType::AEADv2) {
            pkesk_version = PGP_PKSK_V6;
        }
        if (param->is_aead_auth() && (param->ctx.aalg == PGP_AEAD_NONE)) {
            // set default AEAD if not set
            param->ctx.aalg = DEFAULT_AEAD_ALG;
        }
#endif
        ret = encrypted_add_recipient(ctx, dst, recipient, enckey, pkesk_version);
        if (ret) {
            goto finish;
        }
    }

    /* Writing sk-encrypted session key(s) */
    for (auto &skesk : param->skesks) {
        skesk.write(*param->pkt.origdst);
    }

    /* Initializing partial packet writer */
    param->pkt.partial = true;
    param->pkt.indeterminate = false;
    if (param->auth_type == rnp::AuthType::AEADv1) {
        param->pkt.tag = PGP_PKT_AEAD_ENCRYPTED;
    } else {
        /* We do not generate PGP_PKT_SE_DATA, leaving this just in case */
        param->pkt.tag =
          param->auth_type == rnp::AuthType::MDC ? PGP_PKT_SE_IP_DATA : PGP_PKT_SE_DATA;
#ifdef ENABLE_CRYPTO_REFRESH
        if (param->auth_type == rnp::AuthType::AEADv2) {
            param->pkt.tag = PGP_PKT_SE_IP_DATA;
        }
#endif
    }

    /* initializing partial data length writer */
    /* we may use intederminate len packet here as well, for compatibility or so on */
    if (!init_streamed_packet(param->pkt, writedst)) {
        /* LCOV_EXCL_START */
        RNP_LOG("failed to init streamed packet");
        ret = RNP_ERROR_BAD_PARAMETERS;
        goto finish;
        /* LCOV_EXCL_END */
    }

    if (param->is_aead_auth()) {
        /* initialize AEAD encryption */
        ret = encrypted_start_aead(param, enckey.data());
    } else {
        /* initialize old CFB or CFB with MDC */
        ret = encrypted_start_cfb(param, enckey.data());
    }
finish:
    ctx.passwords.clear();
    if (ret) {
        encrypted_dst_close(&dst, true);
    }
    return ret;
}

static rnp_result_t
signed_dst_write(pgp_dest_t *dst, const void *buf, size_t len)
{
    pgp_dest_signed_param_t *param = (pgp_dest_signed_param_t *) dst->param;
    dst_write(param->writedst, buf, len);
    return RNP_SUCCESS;
}

static void
cleartext_dst_writeline(pgp_dest_signed_param_t *param,
                        const uint8_t *          buf,
                        size_t                   len,
                        bool                     eol)
{
    const uint8_t *ptr;

    /* dash-escaping line if needed */
    if (param->clr_start && len &&
        ((buf[0] == CH_DASH) || ((len >= 4) && !strncmp((const char *) buf, ST_FROM, 4)))) {
        dst_write(param->writedst, ST_DASHSP, 2);
    }

    /* output data */
    dst_write(param->writedst, buf, len);

    try {
        if (eol) {
            bool hashcrlf = false;
            ptr = buf + len - 1;

            /* skipping trailing characters - space, tab, carriage return, line feed */
            while ((ptr >= buf) && ((*ptr == CH_SPACE) || (*ptr == CH_TAB) ||
                                    (*ptr == CH_CR) || (*ptr == CH_LF))) {
                if (*ptr == CH_LF) {
                    hashcrlf = true;
                }
                ptr--;
            }

            /* hashing line body and \r\n */
            param->hashes.add(buf, ptr + 1 - buf);
            if (hashcrlf) {
                param->hashes.add(ST_CRLF, 2);
            }
            param->clr_start = hashcrlf;
        } else if (len > 0) {
            /* hashing just line's data */
            param->hashes.add(buf, len);
            param->clr_start = false;
        }
    } catch (const std::exception &e) {
        RNP_LOG("failed to hash data: %s", e.what()); // LCOV_EXCL_LINE
    }
}

static size_t
cleartext_dst_scanline(const uint8_t *buf, size_t len, bool *eol)
{
    for (const uint8_t *ptr = buf, *end = buf + len; ptr < end; ptr++) {
        if (*ptr == CH_LF) {
            if (eol) {
                *eol = true;
            }
            return ptr - buf + 1;
        }
    }

    if (eol) {
        *eol = false;
    }
    return len;
}

static rnp_result_t
cleartext_dst_write(pgp_dest_t *dst, const void *buf, size_t len)
{
    const uint8_t *          linebg = (const uint8_t *) buf;
    size_t                   linelen;
    size_t                   cplen;
    bool                     eol;
    pgp_dest_signed_param_t *param = (pgp_dest_signed_param_t *) dst->param;

    if (param->clr_buflen > 0) {
        /* number of edge cases may happen here */
        linelen = cleartext_dst_scanline(linebg, len, &eol);

        if (param->clr_buflen + linelen < sizeof(param->clr_buf)) {
            /* fits into buffer */
            memcpy(param->clr_buf + param->clr_buflen, linebg, linelen);
            param->clr_buflen += linelen;
            if (!eol) {
                /* do not write the line if we don't have whole */
                return RNP_SUCCESS;
            }

            cleartext_dst_writeline(param, param->clr_buf, param->clr_buflen, true);
            param->clr_buflen = 0;
        } else {
            /* we have line longer than 4k */
            cplen = sizeof(param->clr_buf) - param->clr_buflen;
            memcpy(param->clr_buf + param->clr_buflen, linebg, cplen);
            cleartext_dst_writeline(param, param->clr_buf, sizeof(param->clr_buf), false);

            if (eol || (linelen >= sizeof(param->clr_buf))) {
                cleartext_dst_writeline(param, linebg + cplen, linelen - cplen, eol);
                param->clr_buflen = 0;
            } else {
                param->clr_buflen = linelen - cplen;
                memcpy(param->clr_buf, linebg + cplen, param->clr_buflen);
                return RNP_SUCCESS;
            }
        }

        linebg += linelen;
        len -= linelen;
    }

    /* if we get here then we don't have data in param->clr_buf */
    while (len > 0) {
        linelen = cleartext_dst_scanline(linebg, len, &eol);

        if (!eol && (linelen < sizeof(param->clr_buf))) {
            memcpy(param->clr_buf, linebg, linelen);
            param->clr_buflen = linelen;
            return RNP_SUCCESS;
        }

        cleartext_dst_writeline(param, linebg, linelen, eol);
        linebg += linelen;
        len -= linelen;
    }

    return RNP_SUCCESS;
}

static void
signed_fill_signature(pgp_dest_signed_param_t &param,
                      pgp::pkt::Signature &    sig,
                      pgp_dest_signer_info_t & signer)
{
    /* fill signature fields, assuming sign_init was called on it */
    if (signer.sigcreate) {
        sig.set_creation(signer.sigcreate);
    }
    sig.set_expiration(signer.sigexpire);
    sig.fill_hashed_data();

    auto listh = param.hashes.get(sig.halg);
    if (!listh) {
        /* LCOV_EXCL_START */
        RNP_LOG("failed to obtain hash");
        throw rnp::rnp_exception(RNP_ERROR_BAD_STATE);
        /* LCOV_EXCL_END */
    }

    /* decrypt the secret key if needed */
    rnp::KeyLocker keylock(*signer.key);
    if (signer.key->encrypted() &&
        !signer.key->unlock(*param.password_provider, PGP_OP_SIGN)) {
        RNP_LOG("wrong secret key password");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PASSWORD);
    }
    /* calculate the signature */
    auto hdr = param.has_lhdr ? &param.lhdr : NULL;
    signature_calculate(
      sig, *signer.key->pkt().material, *listh->clone(), param.ctx->sec_ctx, hdr);
}

static rnp_result_t
signed_write_signature(pgp_dest_signed_param_t *param,
                       pgp_dest_signer_info_t * signer,
                       pgp_dest_t *             writedst)
{
    try {
        pgp::pkt::Signature sig;
        if (signer->onepass.version) {
            signer->key->sign_init(param->ctx->sec_ctx.rng,
                                   sig,
                                   signer->onepass.halg,
                                   param->ctx->sec_ctx.time(),
                                   signer->key->version());
            sig.palg = signer->onepass.palg;
            sig.set_type(signer->onepass.type);
        } else {
            signer->key->sign_init(param->ctx->sec_ctx.rng,
                                   sig,
                                   signer->halg,
                                   param->ctx->sec_ctx.time(),
                                   signer->key->version());
            /* line below should be checked */
            sig.set_type(param->ctx->detached ? PGP_SIG_BINARY : PGP_SIG_TEXT);
        }
        signed_fill_signature(*param, sig, *signer);
        sig.write(*writedst);
        return writedst->werr;
    } catch (const rnp::rnp_exception &e) {
        return e.code();
    } catch (const std::exception &e) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to write signature: %s", e.what());
        return RNP_ERROR_WRITE;
        /* LCOV_EXCL_END */
    }
}

static rnp_result_t
signed_dst_finish(pgp_dest_t *dst)
{
    rnp_result_t             ret;
    pgp_dest_signed_param_t *param = (pgp_dest_signed_param_t *) dst->param;

    /* attached signature, we keep onepasses in order of signatures */
    for (auto &sinfo : param->siginfos) {
        if ((ret = signed_write_signature(param, &sinfo, param->writedst))) {
            RNP_LOG("failed to calculate signature");
            return ret;
        }
    }

    return RNP_SUCCESS;
}

static rnp_result_t
signed_detached_dst_finish(pgp_dest_t *dst)
{
    rnp_result_t             ret;
    pgp_dest_signed_param_t *param = (pgp_dest_signed_param_t *) dst->param;

    /* just calculating and writing signatures to the output */
    for (auto &sinfo : param->siginfos) {
        if ((ret = signed_write_signature(param, &sinfo, param->writedst))) {
            RNP_LOG("failed to calculate detached signature");
            return ret;
        }
    }

    return RNP_SUCCESS;
}

static rnp_result_t
cleartext_dst_finish(pgp_dest_t *dst)
{
    pgp_dest_signed_param_t *param = (pgp_dest_signed_param_t *) dst->param;

    /* writing cached line if any */
    if (param->clr_buflen > 0) {
        cleartext_dst_writeline(param, param->clr_buf, param->clr_buflen, true);
    }
    /* trailing \r\n which is not hashed */
    dst_write(param->writedst, ST_CRLF, 2);

    /* writing signatures to the armored stream, which outputs to param->writedst */
    try {
        rnp::ArmoredDest armor(*param->writedst, PGP_ARMORED_SIGNATURE);
        armor.set_discard(true);
        for (auto &sinfo : param->siginfos) {
            auto ret = signed_write_signature(param, &sinfo, &armor.dst());
            if (ret) {
                return ret;
            }
        }
        armor.set_discard(false);
        return RNP_SUCCESS;
    } catch (const std::exception &e) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to write armored signature: %s", e.what());
        return RNP_ERROR_WRITE;
        /* LCOV_EXCL_END */
    }
}

static void
signed_dst_close(pgp_dest_t *dst, bool discard)
{
    pgp_dest_signed_param_t *param = (pgp_dest_signed_param_t *) dst->param;
    delete param;
    dst->param = NULL;
}

static void
signed_dst_update(pgp_dest_t *dst, const void *buf, size_t len)
{
    pgp_dest_signed_param_t *param = (pgp_dest_signed_param_t *) dst->param;
    param->hashes.add(buf, len);
}

static void
signed_dst_set_literal_hdr(pgp_dest_t &src, const pgp_literal_hdr_t &hdr)
{
    auto param = static_cast<pgp_dest_signed_param_t *>(src.param);
    param->lhdr = hdr;
    param->has_lhdr = true;
}

static rnp_result_t
signed_add_signer(pgp_dest_signed_param_t &param, rnp_signer_info_t &signer, bool last)
{
    pgp_dest_signer_info_t sinfo = {};

    if (!signer.key->material() || !signer.key->is_secret()) {
        RNP_LOG("secret key required for signing");
        return RNP_ERROR_BAD_PARAMETERS;
    }
    /* validate signing key material if didn't before */
    signer.key->material()->validate(param.ctx->sec_ctx, false);
    if (!signer.key->pkt().material->valid()) {
        RNP_LOG("attempt to sign to the key with invalid material");
        return RNP_ERROR_NO_SUITABLE_KEY;
    }

    /* copy fields */
    sinfo.key = signer.key;
    sinfo.sigcreate = signer.sigcreate;
    sinfo.sigexpire = signer.sigexpire;

    /* Add hash to the list */
    sinfo.halg = signer.key->material()->adjust_hash(signer.halg);
    try {
        param.hashes.add_alg(sinfo.halg);
    } catch (const std::exception &e) {
        /* LCOV_EXCL_START */
        RNP_LOG("%s", e.what());
        return RNP_ERROR_BAD_PARAMETERS;
        /* LCOV_EXCL_END */
    }

    // Do not add onepass for detached/clearsign
    if (param.ctx->detached || param.ctx->clearsign) {
        sinfo.onepass.version = 0;
        try {
            param.siginfos.push_back(sinfo);
            return RNP_SUCCESS;
        } catch (const std::exception &e) {
            /* LCOV_EXCL_START */
            RNP_LOG("%s", e.what());
            return RNP_ERROR_OUT_OF_MEMORY;
            /* LCOV_EXCL_END */
        }
    }

    // Setup and add onepass
    sinfo.onepass.version = 3;
    sinfo.onepass.type = PGP_SIG_BINARY;
    sinfo.onepass.halg = sinfo.halg;
    sinfo.onepass.palg = sinfo.key->alg();
    sinfo.onepass.keyid = sinfo.key->keyid();
    sinfo.onepass.nested = false;
    try {
        param.siginfos.push_back(sinfo);
    } catch (const std::exception &e) {
        /* LCOV_EXCL_START */
        RNP_LOG("%s", e.what());
        return RNP_ERROR_OUT_OF_MEMORY;
        /* LCOV_EXCL_END */
    }

    // write onepasses in reverse order so signature order will match signers list
    if (!last) {
        return RNP_SUCCESS;
    }
    try {
        for (auto it = param.siginfos.rbegin(); it != param.siginfos.rend(); it++) {
            pgp_dest_signer_info_t &sinfo = *it;
            sinfo.onepass.nested = &sinfo == &param.siginfos.front();
            sinfo.onepass.write(*param.writedst);
        }
        return param.writedst->werr;
    } catch (const std::exception &e) {
        return RNP_ERROR_WRITE; // LCOV_EXCL_LINE
    }
}

static rnp_result_t
init_signed_dst(rnp_ctx_t &ctx, pgp_dest_t &dst, pgp_dest_t &writedst)
{
    pgp_dest_signed_param_t *param = NULL;
    rnp_result_t             ret = RNP_ERROR_GENERIC;

    if (!init_dst_common(&dst, 0)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    try {
        param = new pgp_dest_signed_param_t();
    } catch (const std::exception &e) {
        /* LCOV_EXCL_START */
        RNP_LOG("%s", e.what());
        return RNP_ERROR_OUT_OF_MEMORY;
        /* LCOV_EXCL_END */
    }

    dst.param = param;
    param->writedst = &writedst;
    param->ctx = &ctx;
    param->password_provider = &ctx.pass_provider;
    if (param->ctx->clearsign) {
        dst.type = PGP_STREAM_CLEARTEXT;
        dst.write = cleartext_dst_write;
        dst.finish = cleartext_dst_finish;
        param->clr_start = true;
    } else {
        dst.type = PGP_STREAM_SIGNED;
        dst.write = signed_dst_write;
        dst.finish = param->ctx->detached ? signed_detached_dst_finish : signed_dst_finish;
    }
    dst.close = signed_dst_close;

    /* Getting signer's infos, writing one-pass signatures if needed */
    for (auto &sg : ctx.signers) {
        ret = signed_add_signer(*param, sg, &sg == &ctx.signers.back());
        if (ret) {
            RNP_LOG("failed to add one-pass signature for signer");
            goto finish;
        }
    }

    /* Do we have any signatures? */
    if (param->hashes.hashes.empty()) {
        ret = RNP_ERROR_BAD_PARAMETERS;
        goto finish;
    }

    /* Writing headers for cleartext signed document */
    if (param->ctx->clearsign) {
        dst_write(param->writedst, ST_CLEAR_BEGIN, strlen(ST_CLEAR_BEGIN));
        dst_write(param->writedst, ST_CRLF, strlen(ST_CRLF));
        dst_write(param->writedst, ST_HEADER_HASH, strlen(ST_HEADER_HASH));

        for (const auto &hash : param->hashes.hashes) {
            auto hname = rnp::Hash::name(hash->alg());
            dst_write(param->writedst, hname, strlen(hname));
            if (&hash != &param->hashes.hashes.back()) {
                dst_write(param->writedst, ST_COMMA, 1);
            }
        }

        dst_write(param->writedst, ST_CRLFCRLF, strlen(ST_CRLFCRLF));
    }

    ret = RNP_SUCCESS;
finish:
    if (ret != RNP_SUCCESS) {
        signed_dst_close(&dst, true);
    }

    return ret;
}

static rnp_result_t
compressed_dst_write(pgp_dest_t *dst, const void *buf, size_t len)
{
    pgp_dest_compressed_param_t *param = (pgp_dest_compressed_param_t *) dst->param;

    if (!param) {
        RNP_LOG("wrong param");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    if ((param->alg == PGP_C_ZIP) || (param->alg == PGP_C_ZLIB)) {
        param->z.next_in = (unsigned char *) buf;
        param->z.avail_in = len;
        param->z.next_out = param->cache + param->len;
        param->z.avail_out = sizeof(param->cache) - param->len;

        while (param->z.avail_in > 0) {
            int zret = deflate(&param->z, Z_NO_FLUSH);
            /* Z_OK, Z_BUF_ERROR are ok for us, Z_STREAM_END will not happen here */
            if (zret == Z_STREAM_ERROR) {
                RNP_LOG("wrong deflate state");
                return RNP_ERROR_BAD_STATE;
            }

            /* writing only full blocks, the rest will be written in close */
            if (param->z.avail_out == 0) {
                dst_write(param->pkt.writedst, param->cache, sizeof(param->cache));
                param->len = 0;
                param->z.next_out = param->cache;
                param->z.avail_out = sizeof(param->cache);
            }
        }

        param->len = sizeof(param->cache) - param->z.avail_out;
        return RNP_SUCCESS;
    } else if (param->alg == PGP_C_BZIP2) {
#ifdef HAVE_BZLIB_H
        param->bz.next_in = (char *) buf;
        param->bz.avail_in = len;
        param->bz.next_out = (char *) (param->cache + param->len);
        param->bz.avail_out = sizeof(param->cache) - param->len;

        while (param->bz.avail_in > 0) {
            int zret = BZ2_bzCompress(&param->bz, BZ_RUN);
            if (zret < 0) {
                RNP_LOG("error %d", zret);
                return RNP_ERROR_BAD_STATE;
            }

            /* writing only full blocks, the rest will be written in close */
            if (param->bz.avail_out == 0) {
                dst_write(param->pkt.writedst, param->cache, sizeof(param->cache));
                param->len = 0;
                param->bz.next_out = (char *) param->cache;
                param->bz.avail_out = sizeof(param->cache);
            }
        }

        param->len = sizeof(param->cache) - param->bz.avail_out;
        return RNP_SUCCESS;
#else
        return RNP_ERROR_NOT_IMPLEMENTED;
#endif
    } else {
        RNP_LOG("unknown algorithm");
        return RNP_ERROR_BAD_PARAMETERS;
    }
}

static rnp_result_t
compressed_dst_finish(pgp_dest_t *dst)
{
    int                          zret;
    pgp_dest_compressed_param_t *param = (pgp_dest_compressed_param_t *) dst->param;

    if ((param->alg == PGP_C_ZIP) || (param->alg == PGP_C_ZLIB)) {
        param->z.next_in = Z_NULL;
        param->z.avail_in = 0;
        param->z.next_out = param->cache + param->len;
        param->z.avail_out = sizeof(param->cache) - param->len;
        do {
            zret = deflate(&param->z, Z_FINISH);

            if (zret == Z_STREAM_ERROR) {
                RNP_LOG("wrong deflate state");
                return RNP_ERROR_BAD_STATE;
            }

            if (param->z.avail_out == 0) {
                dst_write(param->pkt.writedst, param->cache, sizeof(param->cache));
                param->len = 0;
                param->z.next_out = param->cache;
                param->z.avail_out = sizeof(param->cache);
            }
        } while (zret != Z_STREAM_END);

        param->len = sizeof(param->cache) - param->z.avail_out;
        dst_write(param->pkt.writedst, param->cache, param->len);
    }
#ifdef HAVE_BZLIB_H
    if (param->alg == PGP_C_BZIP2) {
        param->bz.next_in = NULL;
        param->bz.avail_in = 0;
        param->bz.next_out = (char *) (param->cache + param->len);
        param->bz.avail_out = sizeof(param->cache) - param->len;

        do {
            zret = BZ2_bzCompress(&param->bz, BZ_FINISH);
            if (zret < 0) {
                RNP_LOG("wrong bzip2 state %d", zret);
                return RNP_ERROR_BAD_STATE;
            }

            /* writing only full blocks, the rest will be written in close */
            if (param->bz.avail_out == 0) {
                dst_write(param->pkt.writedst, param->cache, sizeof(param->cache));
                param->len = 0;
                param->bz.next_out = (char *) param->cache;
                param->bz.avail_out = sizeof(param->cache);
            }
        } while (zret != BZ_STREAM_END);

        param->len = sizeof(param->cache) - param->bz.avail_out;
        dst_write(param->pkt.writedst, param->cache, param->len);
    }
#endif

    if (param->pkt.writedst->werr) {
        return param->pkt.writedst->werr;
    }

    return finish_streamed_packet(&param->pkt);
}

static void
compressed_dst_close(pgp_dest_t *dst, bool discard)
{
    pgp_dest_compressed_param_t *param = (pgp_dest_compressed_param_t *) dst->param;

    if (!param) {
        return;
    }

    if (param->zstarted) {
        if ((param->alg == PGP_C_ZIP) || (param->alg == PGP_C_ZLIB)) {
            deflateEnd(&param->z);
        }
#ifdef HAVE_BZLIB_H
        if (param->alg == PGP_C_BZIP2) {
            BZ2_bzCompressEnd(&param->bz);
        }
#endif
    }

    close_streamed_packet(&param->pkt, discard);
    free(param);
    dst->param = NULL;
}

static rnp_result_t
init_compressed_dst(rnp_ctx_t &ctx, pgp_dest_t &dst, pgp_dest_t &writedst)
{
    pgp_dest_compressed_param_t *param = NULL;
    rnp_result_t                 ret = RNP_ERROR_GENERIC;
    uint8_t                      buf = 0;
    int                          zret = 0;

    if (!init_dst_common(&dst, sizeof(*param))) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }

    param = (pgp_dest_compressed_param_t *) dst.param;
    dst.write = compressed_dst_write;
    dst.finish = compressed_dst_finish;
    dst.close = compressed_dst_close;
    dst.type = PGP_STREAM_COMPRESSED;
    param->alg = (pgp_compression_type_t) ctx.zalg;
    param->pkt.partial = true;
    param->pkt.indeterminate = false;
    param->pkt.tag = PGP_PKT_COMPRESSED;

    /* initializing partial length or indeterminate packet, writing header */
    if (!init_streamed_packet(param->pkt, writedst)) {
        /* LCOV_EXCL_START */
        RNP_LOG("failed to init streamed packet");
        ret = RNP_ERROR_BAD_PARAMETERS;
        goto finish;
        /* LCOV_EXCL_END */
    }

    /* compression algorithm */
    buf = param->alg;
    dst_write(param->pkt.writedst, &buf, 1);

    /* initializing compression */
    switch (param->alg) {
    case PGP_C_ZIP:
    case PGP_C_ZLIB:
        (void) memset(&param->z, 0x0, sizeof(param->z));
        if (param->alg == PGP_C_ZIP) {
            zret = deflateInit2(&param->z, ctx.zlevel, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
        } else {
            zret = deflateInit(&param->z, ctx.zlevel);
        }

        if (zret != Z_OK) {
            RNP_LOG("failed to init zlib, error %d", zret);
            ret = RNP_ERROR_NOT_SUPPORTED;
            goto finish;
        }
        break;
#ifdef HAVE_BZLIB_H
    case PGP_C_BZIP2:
        (void) memset(&param->bz, 0x0, sizeof(param->bz));
        zret = BZ2_bzCompressInit(&param->bz, ctx.zlevel, 0, 0);
        if (zret != BZ_OK) {
            RNP_LOG("failed to init bz, error %d", zret);
            ret = RNP_ERROR_NOT_SUPPORTED;
            goto finish;
        }
        break;
#endif
    default:
        RNP_LOG("unknown compression algorithm");
        ret = RNP_ERROR_NOT_SUPPORTED;
        goto finish;
    }
    param->zstarted = true;
    ret = RNP_SUCCESS;
finish:
    if (ret != RNP_SUCCESS) {
        compressed_dst_close(&dst, true);
    }
    return ret;
}

static rnp_result_t
literal_dst_write(pgp_dest_t *dst, const void *buf, size_t len)
{
    pgp_dest_packet_param_t *param = (pgp_dest_packet_param_t *) dst->param;

    if (!param) {
        /* LCOV_EXCL_START */
        RNP_LOG("wrong param");
        return RNP_ERROR_BAD_PARAMETERS;
        /* LCOV_EXCL_END */
    }

    dst_write(param->writedst, buf, len);
    return RNP_SUCCESS;
}

static rnp_result_t
literal_dst_finish(pgp_dest_t *dst)
{
    return finish_streamed_packet((pgp_dest_packet_param_t *) dst->param);
}

static void
literal_dst_close(pgp_dest_t *dst, bool discard)
{
    pgp_dest_packet_param_t *param = (pgp_dest_packet_param_t *) dst->param;

    if (!param) {
        return; // LCOV_EXCL_LINE
    }

    close_streamed_packet(param, discard);
    free(param);
    dst->param = NULL;
}

static void
build_literal_hdr(const rnp_ctx_t &ctx, pgp_literal_hdr_t &hdr)
{
    /* content type - forcing binary now */
    hdr.format = 'b';
    /* filename */
    size_t flen = ctx.filename.size();
    if (flen > 255) {
        RNP_LOG("filename too long, truncating");
        flen = 255;
    }
    hdr.fname_len = flen;
    memcpy(hdr.fname, ctx.filename.c_str(), flen);
    /* timestamp */
    hdr.timestamp = ctx.filemtime;
}

static rnp_result_t
init_literal_dst(pgp_literal_hdr_t &hdr, pgp_dest_t &dst, pgp_dest_t &writedst)
{
    pgp_dest_packet_param_t *param = NULL;

    if (!init_dst_common(&dst, sizeof(*param))) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }

    param = (pgp_dest_packet_param_t *) dst.param;
    dst.write = literal_dst_write;
    dst.finish = literal_dst_finish;
    dst.close = literal_dst_close;
    dst.type = PGP_STREAM_LITERAL;
    param->partial = true;
    param->indeterminate = false;
    param->tag = PGP_PKT_LITDATA;

    /* initializing partial length or indeterminate packet, writing header */
    if (!init_streamed_packet(*param, writedst)) {
        /* LCOV_EXCL_START */
        RNP_LOG("failed to init streamed packet");
        literal_dst_close(&dst, true);
        return RNP_ERROR_BAD_PARAMETERS;
        /* LCOV_EXCL_END */
    }
    /* content type - forcing binary now */
    uint8_t buf[4] = {0};
    buf[0] = hdr.format;
    /* filename length */
    buf[1] = hdr.fname_len;
    dst_write(param->writedst, buf, 2);
    /* filename */
    if (hdr.fname_len) {
        dst_write(param->writedst, hdr.fname, hdr.fname_len);
    }
    /* timestamp */
    write_uint32(buf, hdr.timestamp);
    dst_write(param->writedst, buf, 4);
    return RNP_SUCCESS;
}

static rnp_result_t
process_stream_sequence(pgp_source_t &src,
                        pgp_dest_t *  streams,
                        unsigned      count,
                        pgp_dest_t *  sstream,
                        pgp_dest_t *  wstream)
{
    std::unique_ptr<uint8_t[]> readbuf(new (std::nothrow) uint8_t[PGP_INPUT_CACHE_SIZE]);
    if (!readbuf) {
        /* LCOV_EXCL_START */
        RNP_LOG("allocation failure");
        return RNP_ERROR_OUT_OF_MEMORY;
        /* LCOV_EXCL_END */
    }

    /* processing source stream */
    while (!src.eof_) {
        size_t read = 0;
        if (!src.read(readbuf.get(), PGP_INPUT_CACHE_SIZE, &read)) {
            RNP_LOG("failed to read from source");
            return RNP_ERROR_READ;
        } else if (!read) {
            continue;
        }

        if (sstream) {
            signed_dst_update(sstream, readbuf.get(), read);
        }

        if (wstream) {
            dst_write(wstream, readbuf.get(), read);

            for (int i = count - 1; i >= 0; i--) {
                if (streams[i].werr) {
                    RNP_LOG("failed to process data");
                    return RNP_ERROR_WRITE;
                }
            }
        }
    }

    /* finalizing destinations */
    for (int i = count - 1; i >= 0; i--) {
        rnp_result_t ret = dst_finish(&streams[i]);
        if (ret) {
            RNP_LOG("failed to finish stream");
            return ret;
        }
    }
    return RNP_SUCCESS;
}

rnp_result_t
rnp_sign_src(rnp_ctx_t &ctx, pgp_source_t &src, pgp_dest_t &dst)
{
    /* stack of the streams would be as following:
       [armoring stream] - if armoring is enabled
       [compressing stream, partial writing stream] - compression is enabled, and not detached
       signing stream
       literal data stream, partial writing stream - if not detached or cleartext signature
    */
    pgp_dest_t   dests[4];
    size_t       destc = 0;
    rnp_result_t ret = RNP_ERROR_GENERIC;
    pgp_dest_t * wstream = NULL;
    pgp_dest_t * sstream = NULL;

    /* pushing armoring stream, which will write to the output */
    if (ctx.armor && !ctx.clearsign) {
        pgp_armored_msg_t msgt = ctx.detached ? PGP_ARMORED_SIGNATURE : PGP_ARMORED_MESSAGE;
        ret = init_armored_dst(&dests[destc], &dst, msgt);
        if (ret) {
            goto finish;
        }
        destc++;
    }

    /* if compression is enabled then pushing compressing stream */
    if (!ctx.detached && !ctx.clearsign && (ctx.zlevel > 0)) {
        if ((ret = init_compressed_dst(ctx, dests[destc], destc ? dests[destc - 1] : dst))) {
            goto finish;
        }
        destc++;
    }

    /* pushing signing stream, which will use handler->ctx to distinguish between
     * attached/detached/cleartext signature */
    if ((ret = init_signed_dst(ctx, dests[destc], destc ? dests[destc - 1] : dst))) {
        goto finish;
    }
    if (!ctx.clearsign) {
        sstream = &dests[destc];
    }
    if (!ctx.detached) {
        wstream = &dests[destc];
    }
    destc++;

    /* pushing literal data stream, if not detached/cleartext signature */
    if (!ctx.no_wrap && !ctx.detached && !ctx.clearsign) {
        pgp_literal_hdr_t hdr{};
        build_literal_hdr(ctx, hdr);

        if ((ret = init_literal_dst(hdr, dests[destc], dests[destc - 1]))) {
            goto finish;
        }
        signed_dst_set_literal_hdr(dests[destc - 1], hdr);
        wstream = &dests[destc];
        destc++;
    }

    if (ctx.clearsign) {
        /* See https://dev.gnupg.org/T6615 for the details */
        pgp_literal_hdr_t hdr{};
        hdr.format = 't';
        hdr.fname_len = 0;
        hdr.timestamp = 0;
        signed_dst_set_literal_hdr(dests[destc - 1], hdr);
    }

    /* process source with streams stack */
    ret = process_stream_sequence(src, dests, destc, sstream, wstream);
finish:
    for (auto i = destc; i > 0; i--) {
        dst_close(&dests[i - 1], ret);
    }
    return ret;
}

rnp_result_t
rnp_encrypt_sign_src(rnp_ctx_t &ctx, pgp_source_t &src, pgp_dest_t &dst)
{
    /* stack of the streams would be as following:
       [armoring stream] - if armoring is enabled
       [encrypting stream, partial writing stream]
       [compressing stream, partial writing stream] - compression is enabled
       signing stream
       literal data stream, partial writing stream
    */
    pgp_dest_t   dests[5];
    size_t       destc = 0;
    rnp_result_t ret = RNP_SUCCESS;
    pgp_dest_t * sstream = NULL;

    /* we may use only attached signatures here */
    if (ctx.clearsign || ctx.detached) {
        RNP_LOG("cannot clearsign or sign detached together with encryption");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* pushing armoring stream, which will write to the output */
    if (ctx.armor) {
        if ((ret = init_armored_dst(&dests[destc], &dst, PGP_ARMORED_MESSAGE))) {
            goto finish;
        }
        destc++;
    }

    /* pushing encrypting stream, which will write to the output or armoring stream */
    if ((ret = init_encrypted_dst(ctx, dests[destc], destc ? dests[destc - 1] : dst))) {
        goto finish;
    }
    destc++;

    /* if compression is enabled then pushing compressing stream */
    if (ctx.zlevel > 0) {
        if ((ret = init_compressed_dst(ctx, dests[destc], dests[destc - 1]))) {
            goto finish;
        }
        destc++;
    }

    /* pushing signing stream if we have signers */
    if (!ctx.signers.empty()) {
        if ((ret = init_signed_dst(ctx, dests[destc], dests[destc - 1]))) {
            goto finish;
        }
        sstream = &dests[destc];
        destc++;
    }

    /* pushing literal data stream */
    if (!ctx.no_wrap) {
        pgp_literal_hdr_t hdr{};
        build_literal_hdr(ctx, hdr);

        if ((ret = init_literal_dst(hdr, dests[destc], dests[destc - 1]))) {
            goto finish;
        }

        if (sstream) {
            signed_dst_set_literal_hdr(*sstream, hdr);
        }

        destc++;
    }

    /* process source with streams stack */
    ret = process_stream_sequence(src, dests, destc, sstream, &dests[destc - 1]);
finish:
    for (size_t i = destc; i > 0; i--) {
        dst_close(&dests[i - 1], ret);
    }
    return ret;
}

rnp_result_t
rnp_compress_src(pgp_source_t &src, pgp_dest_t &dst, pgp_compression_type_t zalg, int zlevel)
{
    rnp::SecurityContext    sec_ctx;
    rnp::KeyProvider        key_prov;
    pgp_password_provider_t pass_prov;
    rnp_ctx_t               ctx(sec_ctx, key_prov, pass_prov);

    ctx.zalg = zalg;
    ctx.zlevel = zlevel;

    pgp_dest_t   compressed = {};
    rnp_result_t ret = init_compressed_dst(ctx, compressed, dst);
    if (ret) {
        goto done;
    }
    ret = dst_write_src(&src, &compressed);
done:
    dst_close(&compressed, ret);
    return ret;
}

rnp_result_t
rnp_wrap_src(pgp_source_t &src, pgp_dest_t &dst, const std::string &filename, uint32_t modtime)
{
    rnp::SecurityContext    sec_ctx;
    rnp::KeyProvider        key_prov;
    pgp_password_provider_t pass_prov;
    rnp_ctx_t               ctx(sec_ctx, key_prov, pass_prov);

    ctx.filename = filename;
    ctx.filemtime = modtime;

    pgp_dest_t        literal{};
    pgp_literal_hdr_t hdr{};
    build_literal_hdr(ctx, hdr);

    rnp_result_t ret = init_literal_dst(hdr, literal, dst);
    if (ret) {
        goto done;
    }

    ret = dst_write_src(&src, &literal);
done:
    dst_close(&literal, ret);
    return ret;
}

rnp_result_t
rnp_raw_encrypt_src(pgp_source_t &        src,
                    pgp_dest_t &          dst,
                    const std::string &   password,
                    rnp::SecurityContext &secctx)
{
    rnp::SecurityContext    sec_ctx;
    rnp::KeyProvider        key_prov;
    pgp_password_provider_t pass_prov;
    rnp_ctx_t               ctx(sec_ctx, key_prov, pass_prov);

    pgp_dest_t encrypted = {};

    rnp_result_t ret = RNP_ERROR_GENERIC;
    try {
        ret =
          ctx.add_encryption_password(password, DEFAULT_PGP_HASH_ALG, DEFAULT_PGP_SYMM_ALG);
    } catch (const std::exception &e) {
        /* LCOV_EXCL_START */
        RNP_LOG("%s", e.what());
        goto done;
        /* LCOV_EXCL_END */
    }
    if (ret) {
        goto done;
    }

    ret = init_encrypted_dst(ctx, encrypted, dst);
    if (ret) {
        goto done;
    }

    ret = dst_write_src(&src, &encrypted);
done:
    dst_close(&encrypted, ret);
    return ret;
}
