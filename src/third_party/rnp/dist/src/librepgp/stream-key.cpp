/*
 * Copyright (c) 2018-2022, [Ribose Inc](https://www.ribose.com).
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
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#else
#include "uniwin.h"
#endif
#include <cstring>
#include <time.h>
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#include <cinttypes>
#include "stream-def.h"
#include "stream-key.h"
#include "stream-armor.h"
#include "stream-packet.h"
#include "stream-sig.h"
#include "types.h"
#include "key.hpp"
#include "crypto/signatures.h"
#include "crypto/mem.h"
#include "str-utils.h"
#include <set>
#include <algorithm>
#include <cassert>

static bool
skip_pgp_packets(pgp_source_t &src, const std::set<pgp_pkt_type_t> &pkts)
{
    do {
        int pkt = stream_pkt_type(src);
        if (!pkt) {
            break;
        }
        if (pkt < 0) {
            return false;
        }
        if (pkts.find((pgp_pkt_type_t) pkt) == pkts.end()) {
            return true;
        }
        uint64_t ppos = src.readb;
        if (stream_skip_packet(&src)) {
            RNP_LOG("failed to skip packet at %" PRIu64, ppos);
            return false;
        }
    } while (1);

    return true;
}

static rnp_result_t
process_pgp_key_signatures(pgp_source_t &src, pgp::pkt::Signatures &sigs, bool skiperrors)
{
    int ptag;
    while ((ptag = stream_pkt_type(src)) == PGP_PKT_SIGNATURE) {
        uint64_t sigpos = src.readb;
        try {
            pgp::pkt::Signature sig;
            rnp_result_t        ret = sig.parse(src);
            if (ret) {
                RNP_LOG("failed to parse signature at %" PRIu64, sigpos);
                if (!skiperrors) {
                    return ret;
                }
            } else {
                sigs.emplace_back(std::move(sig));
            }
        } catch (const std::exception &e) {
            RNP_LOG("%s", e.what());
            return RNP_ERROR_OUT_OF_MEMORY;
        }
        if (!skip_pgp_packets(src, {PGP_PKT_TRUST})) {
            return RNP_ERROR_READ;
        }
    }
    return ptag < 0 ? RNP_ERROR_BAD_FORMAT : RNP_SUCCESS;
}

static rnp_result_t
process_pgp_userid(pgp_source_t &src, pgp_transferable_userid_t &uid, bool skiperrors)
{
    rnp_result_t ret;
    uint64_t     uidpos = src.readb;
    try {
        ret = uid.uid.parse(src);
    } catch (const std::exception &e) {
        ret = RNP_ERROR_GENERIC;
    }
    if (ret) {
        RNP_LOG("failed to parse userid at %" PRIu64, uidpos);
        return ret;
    }
    if (!skip_pgp_packets(src, {PGP_PKT_TRUST})) {
        return RNP_ERROR_READ;
    }
    return process_pgp_key_signatures(src, uid.signatures, skiperrors);
}

rnp_result_t
process_pgp_subkey(pgp_source_t &src, pgp_transferable_subkey_t &subkey, bool skiperrors)
{
    int ptag;
    subkey = pgp_transferable_subkey_t();
    uint64_t keypos = src.readb;
    if (!is_subkey_pkt(ptag = stream_pkt_type(src))) {
        RNP_LOG("wrong subkey ptag: %d at %" PRIu64, ptag, keypos);
        return RNP_ERROR_BAD_FORMAT;
    }

    rnp_result_t ret = RNP_ERROR_BAD_FORMAT;
    try {
        ret = subkey.subkey.parse(src);
    } catch (const std::exception &e) {
        RNP_LOG("%s", e.what());
        ret = RNP_ERROR_GENERIC;
    }
    if (ret) {
        RNP_LOG("failed to parse subkey at %" PRIu64, keypos);
        subkey.subkey = {};
        return ret;
    }

    if (!skip_pgp_packets(src, {PGP_PKT_TRUST})) {
        return RNP_ERROR_READ;
    }

    return process_pgp_key_signatures(src, subkey.signatures, skiperrors);
}

rnp_result_t
process_pgp_key_auto(pgp_source_t &          src,
                     pgp_transferable_key_t &key,
                     bool                    allowsub,
                     bool                    skiperrors)
{
    key = {};
    uint64_t srcpos = src.readb;
    int      ptag = stream_pkt_type(src);
    if (is_subkey_pkt(ptag) && allowsub) {
        pgp_transferable_subkey_t subkey;
        rnp_result_t              ret = process_pgp_subkey(src, subkey, skiperrors);
        if (subkey.subkey.tag != PGP_PKT_RESERVED) {
            try {
                key.subkeys.push_back(std::move(subkey));
            } catch (const std::exception &e) {
                RNP_LOG("%s", e.what());
                ret = RNP_ERROR_OUT_OF_MEMORY;
            }
        }
        /* change error code if we didn't process anything at all */
        if (srcpos == src.readb) {
            ret = RNP_ERROR_BAD_STATE;
        }
        return ret;
    }

    rnp_result_t ret = RNP_ERROR_BAD_FORMAT;
    if (!is_primary_key_pkt(ptag)) {
        RNP_LOG("wrong key tag: %d at pos %" PRIu64, ptag, src.readb);
    } else {
        try {
            ret = process_pgp_key(src, key, skiperrors);
        } catch (const rnp::rnp_exception &e) {
            RNP_LOG("%s", e.what());
            ret = e.code();
        } catch (const std::exception &e) {
            RNP_LOG("%s", e.what());
            ret = RNP_ERROR_GENERIC;
        }
    }
    if (skiperrors && (ret == RNP_ERROR_BAD_FORMAT) &&
        !skip_pgp_packets(src,
                          {PGP_PKT_TRUST,
                           PGP_PKT_SIGNATURE,
                           PGP_PKT_USER_ID,
                           PGP_PKT_USER_ATTR,
                           PGP_PKT_PUBLIC_SUBKEY,
                           PGP_PKT_SECRET_SUBKEY})) {
        ret = RNP_ERROR_READ;
    }
    /* change error code if we didn't process anything at all */
    if (srcpos == src.readb) {
        ret = RNP_ERROR_BAD_STATE;
    }
    return ret;
}

rnp_result_t
process_pgp_keys(pgp_source_t &src, pgp_key_sequence_t &keys, bool skiperrors)
{
    bool has_secret = false;
    bool has_public = false;

    keys.keys.clear();
    /* create maybe-armored stream */
    rnp::ArmoredSource armor(
      src, rnp::ArmoredSource::AllowBinary | rnp::ArmoredSource::AllowMultiple);

    /* read sequence of transferable OpenPGP keys as described in RFC 4880, 11.1 - 11.2 */
    while (!armor.error()) {
        /* Allow multiple armored messages in a single stream */
        if (armor.eof() && armor.multiple()) {
            armor.restart();
        }
        if (armor.eof()) {
            break;
        }
        /* Attempt to read the next key */
        pgp_transferable_key_t curkey;
        rnp_result_t ret = process_pgp_key_auto(armor.src(), curkey, false, skiperrors);
        if (ret && (!skiperrors || (ret != RNP_ERROR_BAD_FORMAT))) {
            keys.keys.clear();
            return ret;
        }
        /* check whether we actually read any key or just skipped erroneous packets */
        if (curkey.key.tag == PGP_PKT_RESERVED) {
            continue;
        }
        has_secret |= (curkey.key.tag == PGP_PKT_SECRET_KEY);
        has_public |= (curkey.key.tag == PGP_PKT_PUBLIC_KEY);

        keys.keys.emplace_back(std::move(curkey));
    }

    if (has_secret && has_public) {
        RNP_LOG("warning! public keys are mixed together with secret ones!");
    }

    if (armor.error()) {
        keys.keys.clear();
        return RNP_ERROR_READ;
    }
    return RNP_SUCCESS;
}

rnp_result_t
process_pgp_key(pgp_source_t &src, pgp_transferable_key_t &key, bool skiperrors)
{
    key = pgp_transferable_key_t();
    /* create maybe-armored stream */
    rnp::ArmoredSource armor(
      src, rnp::ArmoredSource::AllowBinary | rnp::ArmoredSource::AllowMultiple);

    /* main key packet */
    uint64_t keypos = armor.readb();
    int      ptag = stream_pkt_type(armor.src());
    if ((ptag <= 0) || !is_primary_key_pkt(ptag)) {
        RNP_LOG("wrong key packet tag: %d at %" PRIu64, ptag, keypos);
        return RNP_ERROR_BAD_FORMAT;
    }

    rnp_result_t ret = key.key.parse(armor.src());
    if (ret) {
        RNP_LOG("failed to parse key pkt at %" PRIu64, keypos);
        key.key = {};
        return ret;
    }

    if (!skip_pgp_packets(armor.src(), {PGP_PKT_TRUST})) {
        return RNP_ERROR_READ;
    }

    /* direct-key signatures */
    if ((ret = process_pgp_key_signatures(armor.src(), key.signatures, skiperrors))) {
        return ret;
    }

    /* user ids/attrs with signatures */
    while ((ptag = stream_pkt_type(armor.src())) > 0) {
        if ((ptag != PGP_PKT_USER_ID) && (ptag != PGP_PKT_USER_ATTR)) {
            break;
        }

        pgp_transferable_userid_t uid;
        ret = process_pgp_userid(armor.src(), uid, skiperrors);
        if ((ret == RNP_ERROR_BAD_FORMAT) && skiperrors &&
            skip_pgp_packets(armor.src(), {PGP_PKT_TRUST, PGP_PKT_SIGNATURE})) {
            /* skip malformed uid */
            continue;
        }
        if (ret) {
            return ret;
        }
        key.userids.push_back(std::move(uid));
    }

    /* subkeys with signatures */
    while ((ptag = stream_pkt_type(armor.src())) > 0) {
        if (!is_subkey_pkt(ptag)) {
            break;
        }

        pgp_transferable_subkey_t subkey;
        ret = process_pgp_subkey(armor.src(), subkey, skiperrors);
        if ((ret == RNP_ERROR_BAD_FORMAT) && skiperrors &&
            skip_pgp_packets(armor.src(), {PGP_PKT_TRUST, PGP_PKT_SIGNATURE})) {
            /* skip malformed subkey */
            continue;
        }
        if (ret) {
            return ret;
        }
        key.subkeys.emplace_back(std::move(subkey));
    }
    return ptag >= 0 ? RNP_SUCCESS : RNP_ERROR_BAD_FORMAT;
}

static rnp_result_t
decrypt_secret_key_v3(pgp_crypt_t *crypt, uint8_t *dec, const uint8_t *enc, size_t len)
{
    size_t idx;
    size_t pos = 0;
    size_t mpilen;
    size_t blsize;

    if (!(blsize = pgp_cipher_block_size(crypt))) {
        RNP_LOG("wrong crypto");
        return RNP_ERROR_BAD_STATE;
    }

    /* 4 RSA secret mpis with cleartext header */
    for (idx = 0; idx < 4; idx++) {
        if (pos + 2 > len) {
            RNP_LOG("bad v3 secret key data");
            return RNP_ERROR_BAD_FORMAT;
        }
        mpilen = (read_uint16(enc + pos) + 7) >> 3;
        memcpy(dec + pos, enc + pos, 2);
        pos += 2;
        if (pos + mpilen > len) {
            RNP_LOG("bad v3 secret key data");
            return RNP_ERROR_BAD_FORMAT;
        }
        pgp_cipher_cfb_decrypt(crypt, dec + pos, enc + pos, mpilen);
        pos += mpilen;
        if (mpilen < blsize) {
            RNP_LOG("bad rsa v3 mpi len");
            return RNP_ERROR_BAD_FORMAT;
        }
        pgp_cipher_cfb_resync(crypt, enc + pos - blsize);
    }

    /* sum16 */
    if (pos + 2 != len) {
        return RNP_ERROR_BAD_FORMAT;
    }
    memcpy(dec + pos, enc + pos, 2);
    return RNP_SUCCESS;
}

static rnp_result_t
parse_secret_key_mpis(pgp_key_pkt_t &key, const uint8_t *mpis, size_t len)
{
    if (!mpis) {
        return RNP_ERROR_NULL_POINTER;
    }

    /* check the cleartext data */
    switch (key.sec_protection.s2k.usage) {
    case PGP_S2KU_NONE:
#if defined(ENABLE_CRYPTO_REFRESH)
        if (key.version == PGP_V6) {
            break; /* checksum removed for v6 and usage byte zero */
        }
        FALLTHROUGH_STATEMENT;
#endif
    case PGP_S2KU_ENCRYPTED: {
        /* calculate and check sum16 of the cleartext */
        if (len < 2) {
            RNP_LOG("No space for checksum.");
            return RNP_ERROR_BAD_FORMAT;
        }
        uint16_t sum = 0;
        len -= 2;
        for (size_t idx = 0; idx < len; idx++) {
            sum += mpis[idx];
        }
        uint16_t expsum = read_uint16(mpis + len);
        if (sum != expsum) {
            RNP_LOG("Wrong key checksum, got 0x%X instead of 0x%X.", (int) sum, (int) expsum);
            return RNP_ERROR_DECRYPT_FAILED;
        }
        break;
    }
    case PGP_S2KU_ENCRYPTED_AND_HASHED: {
        if (len < PGP_SHA1_HASH_SIZE) {
            RNP_LOG("No space for hash");
            return RNP_ERROR_BAD_FORMAT;
        }
        /* calculate and check sha1 hash of the cleartext */
        uint8_t hval[PGP_SHA1_HASH_SIZE];
        try {
            auto hash = rnp::Hash::create(PGP_HASH_SHA1);
            assert(hash->size() == sizeof(hval));
            len -= PGP_SHA1_HASH_SIZE;
            hash->add(mpis, len);
            hash->finish(hval);
        } catch (const std::exception &e) {
            RNP_LOG("hash calculation failed: %s", e.what());
            return RNP_ERROR_BAD_STATE;
        }
        if (memcmp(hval, mpis + len, PGP_SHA1_HASH_SIZE)) {
            return RNP_ERROR_DECRYPT_FAILED;
        }
        break;
    }
    default:
        RNP_LOG("unknown s2k usage: %d", (int) key.sec_protection.s2k.usage);
        return RNP_ERROR_BAD_PARAMETERS;
    }

    try {
        /* parse mpis depending on algorithm */
        pgp_packet_body_t body(mpis, len);

        if (!key.material) {
            RNP_LOG("unknown pk alg : %d", (int) key.alg);
            return RNP_ERROR_BAD_PARAMETERS;
        }
        if (!key.material->parse_secret(body)) {
            return RNP_ERROR_BAD_FORMAT;
        }

        if (body.left()) {
            RNP_LOG("extra data in sec key");
            return RNP_ERROR_BAD_FORMAT;
        }
        return RNP_SUCCESS;
    } catch (const std::exception &e) {
        RNP_LOG("%s", e.what());
        return RNP_ERROR_GENERIC;
    }
}

rnp_result_t
decrypt_secret_key(pgp_key_pkt_t *key, const char *password)
{
    if (!key) {
        return RNP_ERROR_NULL_POINTER;
    }
    if (!is_secret_key_pkt(key->tag)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    /* mark material as not validated as it may be valid for public part */
    key->material->reset_validity();

    /* check whether data is not encrypted */
    if (!key->sec_protection.s2k.usage) {
        return parse_secret_key_mpis(*key, key->sec_data.data(), key->sec_data.size());
    }

    /* check whether secret key data present */
    if (key->sec_data.empty()) {
        RNP_LOG("No secret key data");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* data is encrypted */
    if (!password) {
        return RNP_ERROR_NULL_POINTER;
    }

    if (key->sec_protection.cipher_mode != PGP_CIPHER_MODE_CFB) {
        RNP_LOG("unsupported secret key encryption mode");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    rnp::secure_array<uint8_t, PGP_MAX_KEY_SIZE> keybuf;
    size_t keysize = pgp_key_size(key->sec_protection.symm_alg);
    if (!keysize ||
        !pgp_s2k_derive_key(&key->sec_protection.s2k, password, keybuf.data(), keysize)) {
        RNP_LOG("failed to derive key");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    try {
        rnp::secure_bytes decdata(key->sec_data.size(), 0);
        pgp_crypt_t       crypt;
        if (!pgp_cipher_cfb_start(
              &crypt, key->sec_protection.symm_alg, keybuf.data(), key->sec_protection.iv)) {
            RNP_LOG("failed to start cfb decryption");
            return RNP_ERROR_DECRYPT_FAILED;
        }

        rnp_result_t ret = RNP_ERROR_GENERIC;
        switch (key->version) {
        case PGP_V3:
            if (!is_rsa_key_alg(key->alg)) {
                RNP_LOG("non-RSA v3 key");
                ret = RNP_ERROR_BAD_PARAMETERS;
                break;
            }
            ret = decrypt_secret_key_v3(
              &crypt, decdata.data(), key->sec_data.data(), key->sec_data.size());
            break;
#if defined(ENABLE_CRYPTO_REFRESH)
        case PGP_V6:
            FALLTHROUGH_STATEMENT;
#endif
        case PGP_V4:
        case PGP_V5:
            pgp_cipher_cfb_decrypt(
              &crypt, decdata.data(), key->sec_data.data(), key->sec_data.size());
            ret = RNP_SUCCESS;
            break;
        default:
            ret = RNP_ERROR_BAD_PARAMETERS;
        }

        pgp_cipher_cfb_finish(&crypt);
        if (ret) {
            return ret;
        }

        return parse_secret_key_mpis(*key, decdata.data(), decdata.size());
    } catch (const std::exception &e) {
        RNP_LOG("%s", e.what());
        return RNP_ERROR_GENERIC;
    }
}

static void
write_secret_key_mpis(pgp_packet_body_t &body, pgp_key_pkt_t &key)
{
    /* add mpis */
    key.material->write_secret(body);

#if defined(ENABLE_CRYPTO_REFRESH)
    if (key.version == PGP_V6 && key.sec_protection.s2k.usage == PGP_S2KU_NONE) {
        return; /* checksum removed for v6 and usage byte zero */
    }
#endif

    /* add sum16 if sha1 is not used */
    if (key.sec_protection.s2k.usage != PGP_S2KU_ENCRYPTED_AND_HASHED) {
        uint16_t sum = 0;
        for (size_t i = 0; i < body.size(); i++) {
            sum += body.data()[i];
        }
        body.add_uint16(sum);
        return;
    }

    /* add sha1 hash */
    auto hash = rnp::Hash::create(PGP_HASH_SHA1);
    hash->add(body.data(), body.size());
    assert(hash->size() == PGP_SHA1_HASH_SIZE);
    body.add(hash->finish());
}

rnp_result_t
encrypt_secret_key(pgp_key_pkt_t *key, const char *password, rnp::RNG &rng)
{
    if (!is_secret_key_pkt(key->tag) || !key->material->secret()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (key->sec_protection.s2k.usage &&
        (key->sec_protection.cipher_mode != PGP_CIPHER_MODE_CFB)) {
        RNP_LOG("unsupported secret key encryption mode");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    try {
        /* build secret key data */
        pgp_packet_body_t body(PGP_PKT_RESERVED);
        body.mark_secure();
        write_secret_key_mpis(body, *key);

        /* check whether data is not encrypted */
        if (key->sec_protection.s2k.usage == PGP_S2KU_NONE) {
            secure_clear(key->sec_data.data(), key->sec_data.size());
            key->sec_data.assign(body.data(), body.data() + body.size());
            return RNP_SUCCESS;
        }
        if (key->version < PGP_V4) {
            RNP_LOG("encryption of v3 keys is not supported");
            return RNP_ERROR_BAD_PARAMETERS;
        }

        /* data is encrypted */
        size_t keysize = pgp_key_size(key->sec_protection.symm_alg);
        size_t blsize = pgp_block_size(key->sec_protection.symm_alg);
        if (!keysize || !blsize) {
            RNP_LOG("wrong symm alg");
            return RNP_ERROR_BAD_PARAMETERS;
        }
        /* generate iv and s2k salt */
        rng.get(key->sec_protection.iv, blsize);
        if ((key->sec_protection.s2k.specifier != PGP_S2KS_SIMPLE)) {
            rng.get(key->sec_protection.s2k.salt, PGP_SALT_SIZE);
        }
        /* derive key */
        rnp::secure_array<uint8_t, PGP_MAX_KEY_SIZE> keybuf;
        if (!pgp_s2k_derive_key(&key->sec_protection.s2k, password, keybuf.data(), keysize)) {
            RNP_LOG("failed to derive key");
            return RNP_ERROR_BAD_PARAMETERS;
        }
        /* encrypt sec data */
        pgp_crypt_t crypt;
        if (!pgp_cipher_cfb_start(
              &crypt, key->sec_protection.symm_alg, keybuf.data(), key->sec_protection.iv)) {
            RNP_LOG("failed to start cfb encryption");
            return RNP_ERROR_DECRYPT_FAILED;
        }
        pgp_cipher_cfb_encrypt(&crypt, body.data(), body.data(), body.size());
        pgp_cipher_cfb_finish(&crypt);
        secure_clear(key->sec_data.data(), key->sec_data.size());
        key->sec_data.assign(body.data(), body.data() + body.size());
        /* cleanup cleartext fields */
        key->material->clear_secret();
        return RNP_SUCCESS;
    } catch (const std::exception &e) {
        RNP_LOG("%s", e.what());
        return RNP_ERROR_GENERIC;
    }
}

bool
pgp_userid_pkt_t::operator==(const pgp_userid_pkt_t &src) const
{
    return (tag == src.tag) && (uid == src.uid);
}

bool
pgp_userid_pkt_t::operator!=(const pgp_userid_pkt_t &src) const
{
    return !(*this == src);
}
void
pgp_userid_pkt_t::write(pgp_dest_t &dst) const
{
    if ((tag != PGP_PKT_USER_ID) && (tag != PGP_PKT_USER_ATTR)) {
        RNP_LOG("wrong userid tag");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }

    pgp_packet_body_t pktbody(tag);
    pktbody.add(uid.data(), uid.size());
    pktbody.write(dst);
}

rnp_result_t
pgp_userid_pkt_t::parse(pgp_source_t &src)
{
    /* check the tag */
    int stag = stream_pkt_type(src);
    if ((stag != PGP_PKT_USER_ID) && (stag != PGP_PKT_USER_ATTR)) {
        RNP_LOG("wrong userid tag: %d", stag);
        return RNP_ERROR_BAD_FORMAT;
    }

    pgp_packet_body_t pkt(PGP_PKT_RESERVED);
    rnp_result_t      res = pkt.read(src);
    if (res) {
        return res;
    }

    /* userid type, i.e. tag */
    tag = (pgp_pkt_type_t) stag;
    uid.resize(pkt.size());
    if (pkt.size()) {
        std::memcpy(uid.data(), pkt.data(), pkt.size());
    }
    return RNP_SUCCESS;
}

pgp_key_pkt_t::pgp_key_pkt_t(const pgp_key_pkt_t &src, bool pubonly)
{
    if (pubonly && is_secret_key_pkt(src.tag)) {
        tag = (src.tag == PGP_PKT_SECRET_KEY) ? PGP_PKT_PUBLIC_KEY : PGP_PKT_PUBLIC_SUBKEY;
    } else {
        tag = src.tag;
    }
    version = src.version;
    creation_time = src.creation_time;
    alg = src.alg;
    v3_days = src.v3_days;
    v5_pub_len = src.v5_pub_len;
    pub_data = src.pub_data;
    material = src.material ? src.material->clone() : nullptr;
    if (pubonly) {
        if (material) {
            material->clear_secret();
        }
        sec_data.resize(0);
        v5_s2k_len = 0;
        v5_sec_len = 0;
        sec_protection = {};
        return;
    }
    sec_data = src.sec_data;
    v5_s2k_len = src.v5_s2k_len;
    v5_sec_len = src.v5_sec_len;
    sec_protection = src.sec_protection;
}

pgp_key_pkt_t::pgp_key_pkt_t(pgp_key_pkt_t &&src)
{
    tag = src.tag;
    version = src.version;
    creation_time = src.creation_time;
    alg = src.alg;
    v3_days = src.v3_days;
    pub_data = std::move(src.pub_data);
    material = std::move(src.material);
    sec_data = std::move(src.sec_data);
    v5_s2k_len = src.v5_s2k_len;
    v5_sec_len = src.v5_sec_len;
    v5_pub_len = src.v5_pub_len;
    sec_protection = src.sec_protection;
}

pgp_key_pkt_t &
pgp_key_pkt_t::operator=(pgp_key_pkt_t &&src)
{
    if (this == &src) {
        return *this;
    }
    tag = src.tag;
    version = src.version;
    creation_time = src.creation_time;
    alg = src.alg;
    v3_days = src.v3_days;
    pub_data = std::move(src.pub_data);
    material = std::move(src.material);
    secure_clear(sec_data.data(), sec_data.size());
    sec_data = std::move(src.sec_data);
    sec_protection = src.sec_protection;
    return *this;
}

pgp_key_pkt_t &
pgp_key_pkt_t::operator=(const pgp_key_pkt_t &src)
{
    if (this == &src) {
        return *this;
    }
    tag = src.tag;
    version = src.version;
    creation_time = src.creation_time;
    alg = src.alg;
    v3_days = src.v3_days;
    pub_data = src.pub_data;
    material = src.material ? src.material->clone() : nullptr;
    secure_clear(sec_data.data(), sec_data.size());
    sec_data = std::move(src.sec_data);
    sec_protection = src.sec_protection;
    return *this;
}

pgp_key_pkt_t::~pgp_key_pkt_t()
{
    secure_clear(sec_data.data(), sec_data.size());
}

#if defined(ENABLE_CRYPTO_REFRESH)
uint8_t
pgp_key_pkt_t::s2k_specifier_len(pgp_s2k_specifier_t specifier)
{
    switch (specifier) {
    case PGP_S2KS_SIMPLE:
        return 2;
    case PGP_S2KS_SALTED:
        return 10;
    case PGP_S2KS_ITERATED_AND_SALTED:
        return 11;
    default:
        RNP_LOG("invalid specifier");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}
#endif

void
pgp_key_pkt_t::make_s2k_params(pgp_packet_body_t &hbody)
{
    switch (sec_protection.s2k.usage) {
    case PGP_S2KU_NONE:
        break;
    case PGP_S2KU_ENCRYPTED_AND_HASHED:
    case PGP_S2KU_ENCRYPTED: {
        hbody.add_byte(sec_protection.symm_alg);
#if defined(ENABLE_CRYPTO_REFRESH)
        if (version == PGP_V6) {
            // V6 packages contain length of the following field
            hbody.add_byte(s2k_specifier_len(sec_protection.s2k.specifier));
        }
#endif
        hbody.add(sec_protection.s2k);
        if (sec_protection.s2k.specifier != PGP_S2KS_EXPERIMENTAL) {
            size_t blsize = pgp_block_size(sec_protection.symm_alg);
            if (!blsize) {
                RNP_LOG("wrong block size");
                throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
            }
            hbody.add(sec_protection.iv, blsize);
        }
        break;
    }
    default:
        RNP_LOG("wrong s2k usage");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

void
pgp_key_pkt_t::write(pgp_dest_t &dst)
{
    if (!is_key_pkt(tag)) {
        RNP_LOG("wrong key tag");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
    if (pub_data.empty()) {
        fill_hashed_data();
    }

    pgp_packet_body_t pktbody(tag);
    /* all public key data is written in hashed_data */
    pktbody.add(pub_data);
    /* if we have public key then we do not need further processing */
    if (!is_secret_key_pkt(tag)) {
        pktbody.write(dst);
        return;
    }

    /* secret key fields should be pre-populated in sec_data field */
    if ((sec_protection.s2k.specifier != PGP_S2KS_EXPERIMENTAL) && sec_data.empty()) {
        RNP_LOG("secret key data is not populated");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
    pktbody.add_byte(sec_protection.s2k.usage);
    if (version == PGP_V5) {
        pktbody.add_byte(v5_s2k_len);
    }

    pgp_packet_body_t s2k_params(tag);
    make_s2k_params(s2k_params);
#if defined(ENABLE_CRYPTO_REFRESH)
    if ((version == PGP_V6) && (sec_protection.s2k.usage != PGP_S2KU_NONE)) {
        // V6 packages contain the count of the optional 1-byte parameters
        pktbody.add_byte(s2k_params.size());
    }
#endif
    pktbody.add(s2k_params.data(), s2k_params.size());

    if (version == PGP_V5) {
        pktbody.add_uint32(sec_data.size());
    }
    /* if key is stored on card, or exported via gpg --export-secret-subkeys, then
     * sec_data is empty */
    pktbody.add(sec_data);
    pktbody.write(dst);
}

rnp_result_t
pgp_key_pkt_t::parse(pgp_source_t &src)
{
    /* check the key tag */
    int atag = stream_pkt_type(src);
    if (!is_key_pkt(atag)) {
        RNP_LOG("wrong key packet tag: %d", atag);
        return RNP_ERROR_BAD_FORMAT;
    }

#if defined(ENABLE_CRYPTO_REFRESH) || defined(ENABLE_PQC)
    std::vector<uint8_t> tmpbuf;
#endif

    pgp_packet_body_t pkt((pgp_pkt_type_t) atag);
    /* Read the packet into memory */
    rnp_result_t res = pkt.read(src);
    if (res) {
        return res;
    }
    /* key type, i.e. tag */
    tag = (pgp_pkt_type_t) atag;
    /* version */
    uint8_t ver = 0;
    if (!pkt.get(ver)) {
        RNP_LOG("unable to retrieve key packet version");
        return RNP_ERROR_BAD_FORMAT;
    }
    switch (ver) {
    case PGP_V2:
        FALLTHROUGH_STATEMENT;
    case PGP_V3:
        FALLTHROUGH_STATEMENT;
    case PGP_V4:
        FALLTHROUGH_STATEMENT;
    case PGP_V5:
        break;
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_V6:
        break;
#endif
    default:
        RNP_LOG("wrong key packet version");
        return RNP_ERROR_BAD_FORMAT;
    }
    version = (pgp_version_t) ver;
    /* creation time */
    if (!pkt.get(creation_time)) {
        return RNP_ERROR_BAD_FORMAT;
    }
    /* v3: validity days */
    if ((version < PGP_V4) && !pkt.get(v3_days)) {
        return RNP_ERROR_BAD_FORMAT;
    }
    /* key algorithm */
    uint8_t analg = 0;
    if (!pkt.get(analg)) {
        return RNP_ERROR_BAD_FORMAT;
    }
    alg = (pgp_pubkey_alg_t) analg;
    material = pgp::KeyMaterial::create(alg);
    if (!material) {
        RNP_LOG("unknown key algorithm: %d", (int) alg);
        return RNP_ERROR_BAD_FORMAT;
    }
    switch (version) {
    case PGP_V2:
    case PGP_V3:
        /* v3 keys must be RSA-only */
        if (!is_rsa_key_alg(alg)) {
            RNP_LOG("wrong v3 pk algorithm");
            return RNP_ERROR_BAD_FORMAT;
        }
        break;
    case PGP_V5:
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_V6:
#endif
        /* v5-v6 public key material length  */
        if (!pkt.get(v5_pub_len)) {
            RNP_LOG("failed to get v5 octet count field");
            return RNP_ERROR_BAD_FORMAT;
        }
        if (is_public_key_pkt(atag) && (v5_pub_len != pkt.left())) {
            RNP_LOG("v5 octet count mismatch");
            return RNP_ERROR_BAD_FORMAT;
        }
        break;
    default:;
    }

    /* algorithm specific fields */
    if (!material->parse(pkt)) {
        return RNP_ERROR_BAD_FORMAT;
    }

    /* fill hashed data used for signatures */
    pub_data.assign(pkt.data(), pkt.data() + pkt.size() - pkt.left());

    /* secret key fields if any */
    if (is_secret_key_pkt(tag)) {
        uint8_t usage = 0;
        if (!pkt.get(usage)) {
            RNP_LOG("failed to read key protection");
            return RNP_ERROR_BAD_FORMAT;
        }
#if defined(ENABLE_CRYPTO_REFRESH)
        if (version == PGP_V6 && usage == 255) {
            RNP_LOG(
              "Error when parsing S2K usage: A version 6 packet MUST NOT use the value 255.");
            return RNP_ERROR_BAD_FORMAT;
        }
#endif
        sec_protection.s2k.usage = (pgp_s2k_usage_t) usage;
        sec_protection.cipher_mode = PGP_CIPHER_MODE_CFB;

        /* v5 s2k length, ignored for now */
        if (version == PGP_V5) {
            if (!pkt.get(v5_s2k_len)) {
                RNP_LOG("failed to read v5 s2k len");
                return RNP_ERROR_BAD_FORMAT;
            }
        }
#if defined(ENABLE_CRYPTO_REFRESH)
        if (version == PGP_V6 && sec_protection.s2k.usage != PGP_S2KU_NONE) {
            // V6 packages contain the count of the optional 1-byte parameters
            if (!pkt.get(v5_s2k_len)) {
                RNP_LOG("failed to read key protection");
                return RNP_ERROR_BAD_FORMAT;
            }
        }
#endif

        switch (sec_protection.s2k.usage) {
        case PGP_S2KU_NONE:
            break;
        case PGP_S2KU_ENCRYPTED:
        case PGP_S2KU_ENCRYPTED_AND_HASHED: {
            /* we have s2k */
            uint8_t salg = 0;
            if (!pkt.get(salg)) {
                RNP_LOG("failed to read key protection (symmetric alg)");
                return RNP_ERROR_BAD_FORMAT;
            }
#if defined(ENABLE_CRYPTO_REFRESH)
            if (version == PGP_V6) {
                // V6 packages contain the length of the following field
                uint8_t s2k_specifier_len;
                if (!pkt.get(s2k_specifier_len)) {
                    RNP_LOG("failed to read key protection (s2k specifier length)");
                }
            }
#endif
            if (!pkt.get(sec_protection.s2k)) {
                RNP_LOG("failed to read key protection (s2k)");
                return RNP_ERROR_BAD_FORMAT;
            }
            sec_protection.symm_alg = (pgp_symm_alg_t) salg;
            break;
        }
        default:
            /* old-style: usage is symmetric algorithm identifier */
            sec_protection.symm_alg = (pgp_symm_alg_t) usage;
            sec_protection.s2k.usage = PGP_S2KU_ENCRYPTED;
            sec_protection.s2k.specifier = PGP_S2KS_SIMPLE;
            sec_protection.s2k.hash_alg = PGP_HASH_MD5;
            break;
        }

        /* iv */
        if (sec_protection.s2k.usage &&
            (sec_protection.s2k.specifier != PGP_S2KS_EXPERIMENTAL)) {
            size_t bl_size = pgp_block_size(sec_protection.symm_alg);
            if (!bl_size || !pkt.get(sec_protection.iv, bl_size)) {
                RNP_LOG("failed to read iv");
                return RNP_ERROR_BAD_FORMAT;
            }
        }

        /* v5 secret key fields length */
        if (version == PGP_V5) {
            if (!pkt.get(v5_sec_len)) {
                RNP_LOG("failed to read v5 secret fields length");
                return RNP_ERROR_BAD_FORMAT;
            }
            if (v5_sec_len != pkt.left()) {
                RNP_LOG("v5 secret fields length mismatch");
                return RNP_ERROR_BAD_FORMAT;
            }
        }

        /* encrypted/cleartext secret MPIs are left */
        size_t sec_len = pkt.left();
        sec_data.resize(sec_len);
        if (sec_len && !pkt.get(sec_data.data(), sec_len)) {
            return RNP_ERROR_BAD_STATE;
        }
    }

    if (pkt.left()) {
        RNP_LOG("extra %zu bytes in key packet", pkt.left());
        return RNP_ERROR_BAD_FORMAT;
    }
    return RNP_SUCCESS;
}

void
pgp_key_pkt_t::fill_hashed_data()
{
    /* we don't have a need to write v2-v3 signatures */
    switch (version) {
    case PGP_V4:
        break;
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_V6:
        break;
#endif
    default:
        RNP_LOG("unknown key version %d", (int) version);
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }

    pgp_packet_body_t hbody(PGP_PKT_RESERVED);
    hbody.add_byte(version);
    hbody.add_uint32(creation_time);
    hbody.add_byte(alg);

    /* Algorithm specific fields */
    pgp_packet_body_t alg_spec_fields(PGP_PKT_RESERVED);
    material->write(alg_spec_fields);
#if defined(ENABLE_CRYPTO_REFRESH)
    if (version == PGP_V6) {
        hbody.add_uint32(alg_spec_fields.size());
    }
#endif
    hbody.add(alg_spec_fields.data(), alg_spec_fields.size());
    pub_data.assign(hbody.data(), hbody.data() + hbody.size());
}

pgp_transferable_subkey_t::pgp_transferable_subkey_t(const pgp_transferable_subkey_t &src,
                                                     bool                             pubonly)
{
    subkey = pgp_key_pkt_t(src.subkey, pubonly);
    signatures = src.signatures;
}

pgp_transferable_key_t::pgp_transferable_key_t(const pgp_transferable_key_t &src, bool pubonly)
{
    key = pgp_key_pkt_t(src.key, pubonly);
    userids = src.userids;
    subkeys = src.subkeys;
    signatures = src.signatures;
}
