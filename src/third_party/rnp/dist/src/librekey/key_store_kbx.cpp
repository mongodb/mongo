/*
 * Copyright (c) 2017-2023, [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#else
#include "uniwin.h"
#endif
#include <string.h>
#include <stdint.h>
#include <time.h>
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#include <cinttypes>
#include <cassert>

#include "key.hpp"
#include "kbx_blob.hpp"
#include "rekey/rnp_key_store.h"
#include <librepgp/stream-sig.h>

/* same limit with GnuPG 2.1 */
#define BLOB_SIZE_LIMIT (5 * 1024 * 1024)
/* limit the number of keys/sigs/uids in the blob */
#define BLOB_OBJ_LIMIT 0x8000

#define BLOB_HEADER_SIZE 0x5
#define BLOB_FIRST_SIZE 0x20
#define BLOB_KEY_SIZE 0x1C
#define BLOB_UID_SIZE 0x0C
#define BLOB_SIG_SIZE 0x04
#define BLOB_VALIDITY_SIZE 0x10

uint8_t
kbx_blob_t::ru8(size_t idx)
{
    return image_[idx];
}

uint16_t
kbx_blob_t::ru16(size_t idx)
{
    return read_uint16(image_.data() + idx);
}

uint32_t
kbx_blob_t::ru32(size_t idx)
{
    return read_uint32(image_.data() + idx);
}

kbx_blob_t::kbx_blob_t(std::vector<uint8_t> &data)
{
    if (data.size() < BLOB_HEADER_SIZE) {
        RNP_LOG("Too small KBX blob.");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
    uint32_t len = read_uint32(data.data());
    if (len > BLOB_SIZE_LIMIT) {
        RNP_LOG("Too large KBX blob.");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
    if (len != data.size()) {
        RNP_LOG("KBX blob size mismatch.");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
    image_ = data;
    type_ = (kbx_blob_type_t) ru8(4);
}

bool
kbx_header_blob_t::parse()
{
    if (length() != BLOB_FIRST_SIZE) {
        RNP_LOG("The first blob has wrong length: %" PRIu32 " but expected %d",
                length(),
                (int) BLOB_FIRST_SIZE);
        return false;
    }

    size_t idx = BLOB_HEADER_SIZE;
    version_ = ru8(idx++);
    if (version_ != 1) {
        RNP_LOG("Wrong version, expect 1 but has %" PRIu8, version_);
        return false;
    }

    flags_ = ru16(idx);
    idx += 2;

    // blob should contains a magic KBXf
    if (memcmp(image_.data() + idx, "KBXf", 4)) {
        RNP_LOG("The first blob hasn't got a KBXf magic string");
        return false;
    }
    idx += 4;
    // RFU
    idx += 4;
    // File creation time
    file_created_at_ = ru32(idx);
    idx += 4;
    // Duplicated?
    file_created_at_ = ru32(idx);
    // RFU +4 bytes
    // RFU +4 bytes
    return true;
}

bool
kbx_pgp_blob_t::parse()
{
    /* Skip parsing of X.509 and empty blobs. */
    if (type_ != KBX_PGP_BLOB) {
        return true;
    }
    if (image_.size() < 15 + BLOB_HEADER_SIZE) {
        RNP_LOG("Too few data in the blob.");
        return false;
    }

    size_t idx = BLOB_HEADER_SIZE;
    /* version */
    version_ = ru8(idx++);
    if (version_ != 1) {
        RNP_LOG("Wrong version: %" PRIu8, version_);
        return false;
    }
    /* flags */
    flags_ = ru16(idx);
    idx += 2;
    /* keyblock offset */
    keyblock_offset_ = ru32(idx);
    idx += 4;
    /* keyblock length */
    keyblock_length_ = ru32(idx);
    idx += 4;

    if ((keyblock_offset_ > image_.size()) ||
        (keyblock_offset_ > (UINT32_MAX - keyblock_length_)) ||
        (image_.size() < (keyblock_offset_ + keyblock_length_))) {
        RNP_LOG("Wrong keyblock offset/length, blob size: %zu"
                ", keyblock offset: %" PRIu32 ", length: %" PRIu32,
                image_.size(),
                keyblock_offset_,
                keyblock_length_);
        return false;
    }
    /* number of key blocks */
    size_t nkeys = ru16(idx);
    idx += 2;
    if (nkeys < 1) {
        RNP_LOG("PGP blob should contain at least 1 key");
        return false;
    }
    if (nkeys > BLOB_OBJ_LIMIT) {
        RNP_LOG("Too many keys in the PGP blob");
        return false;
    }

    /* Size of the single key record */
    size_t keys_len = ru16(idx);
    idx += 2;
    if (keys_len < BLOB_KEY_SIZE) {
        RNP_LOG(
          "Key record needs %d bytes, but contains: %zu bytes", (int) BLOB_KEY_SIZE, keys_len);
        return false;
    }

    for (size_t i = 0; i < nkeys; i++) {
        if (image_.size() - idx < keys_len) {
            RNP_LOG("Too few bytes left for key blob");
            return false;
        }

        kbx_pgp_key_t nkey = {};
        /* copy fingerprint */
        memcpy(nkey.fp, &image_[idx], 20);
        idx += 20;
        /* keyid offset */
        nkey.keyid_offset = ru32(idx);
        idx += 4;
        /* flags */
        nkey.flags = ru16(idx);
        idx += 2;
        /* RFU */
        idx += 2;
        /* skip padding bytes if it existed */
        idx += keys_len - BLOB_KEY_SIZE;
        keys_.push_back(std::move(nkey));
    }

    if (image_.size() - idx < 2) {
        RNP_LOG("No data for sn_size");
        return false;
    }
    size_t sn_size = ru16(idx);
    idx += 2;

    if (image_.size() - idx < sn_size) {
        RNP_LOG("SN is %zu, while bytes left are %zu", sn_size, image_.size() - idx);
        return false;
    }

    if (sn_size) {
        sn_ = {image_.begin() + idx, image_.begin() + idx + sn_size};
        idx += sn_size;
    }

    if (image_.size() - idx < 4) {
        RNP_LOG("Too few data for uids");
        return false;
    }
    size_t nuids = ru16(idx);
    if (nuids > BLOB_OBJ_LIMIT) {
        RNP_LOG("Too many uids in the PGP blob");
        return false;
    }

    size_t uids_len = ru16(idx + 2);
    idx += 4;

    if (uids_len < BLOB_UID_SIZE) {
        RNP_LOG("Too few bytes for uid struct: %zu", uids_len);
        return false;
    }

    for (size_t i = 0; i < nuids; i++) {
        if (image_.size() - idx < uids_len) {
            RNP_LOG("Too few bytes to read uid struct.");
            return false;
        }
        kbx_pgp_uid_t nuid = {};
        /* offset */
        nuid.offset = ru32(idx);
        idx += 4;
        /* length */
        nuid.length = ru32(idx);
        idx += 4;
        /* flags */
        nuid.flags = ru16(idx);
        idx += 2;
        /* validity */
        nuid.validity = ru8(idx);
        idx++;
        /* RFU */
        idx++;
        // skip padding bytes if it existed
        idx += uids_len - BLOB_UID_SIZE;

        uids_.push_back(std::move(nuid));
    }

    if (image_.size() - idx < 4) {
        RNP_LOG("No data left for sigs");
        return false;
    }

    size_t nsigs = ru16(idx);
    if (nsigs > BLOB_OBJ_LIMIT) {
        RNP_LOG("Too many sigs in the PGP blob");
        return false;
    }

    size_t sigs_len = ru16(idx + 2);
    idx += 4;

    if (sigs_len < BLOB_SIG_SIZE) {
        RNP_LOG("Too small SIGN structure: %zu", sigs_len);
        return false;
    }

    for (size_t i = 0; i < nsigs; i++) {
        if (image_.size() - idx < sigs_len) {
            RNP_LOG("Too few data for sig");
            return false;
        }

        kbx_pgp_sig_t nsig = {};
        nsig.expired = ru32(idx);
        idx += 4;

        // skip padding bytes if it existed
        idx += (sigs_len - BLOB_SIG_SIZE);

        sigs_.push_back(nsig);
    }

    if (image_.size() - idx < BLOB_VALIDITY_SIZE) {
        RNP_LOG("Too few data for trust/validities");
        return false;
    }

    ownertrust_ = ru8(idx);
    idx++;
    all_validity_ = ru8(idx);
    idx++;
    // RFU
    idx += 2;
    recheck_after_ = ru32(idx);
    idx += 4;
    latest_timestamp_ = ru32(idx);
    idx += 4;
    blob_created_at_ = ru32(idx);
    // do not forget to idx += 4 on further expansion

    // here starts keyblock, UID and reserved space for future usage

    // Maybe we should add checksum verify but GnuPG never checked it
    // Checksum is last 20 bytes of blob and it is SHA-1, if it invalid MD5 and starts from 4
    // zero it is MD5.

    return true;
}

namespace rnp {
namespace {
std::unique_ptr<kbx_blob_t>
kbx_parse_blob(const uint8_t *image, size_t image_len)
{
    std::unique_ptr<kbx_blob_t> blob;
    // a blob shouldn't be less of length + type
    if (image_len < BLOB_HEADER_SIZE) {
        RNP_LOG("Blob size is %zu but it shouldn't be less of header", image_len);
        return blob;
    }

    try {
        std::vector<uint8_t> data(image, image + image_len);
        kbx_blob_type_t      type = (kbx_blob_type_t) image[4];

        switch (type) {
        case KBX_EMPTY_BLOB:
            blob = std::unique_ptr<kbx_blob_t>(new kbx_blob_t(data));
            break;
        case KBX_HEADER_BLOB:
            blob = std::unique_ptr<kbx_blob_t>(new kbx_header_blob_t(data));
            break;
        case KBX_PGP_BLOB:
            blob = std::unique_ptr<kbx_blob_t>(new kbx_pgp_blob_t(data));
            break;
        case KBX_X509_BLOB:
            // current we doesn't parse X509 blob, so, keep it as is
            blob = std::unique_ptr<kbx_blob_t>(new kbx_blob_t(data));
            break;
        // unsupported blob type
        default:
            RNP_LOG("Unsupported blob type: %d", (int) type);
            return blob;
        }

        if (!blob->parse()) {
            return NULL;
        }
    } catch (const std::exception &e) {
        /* LCOV_EXCL_START */
        RNP_LOG("%s", e.what());
        return NULL;
        /* LCOV_EXCL_END */
    }
    return blob;
}
} // namespace

bool
KeyStore::load_kbx(pgp_source_t &src, const KeyProvider *key_provider)
{
    try {
        MemorySource mem(src);
        size_t       has_bytes = mem.size();
        uint8_t *    buf = (uint8_t *) mem.memory();

        if (has_bytes < BLOB_FIRST_SIZE) {
            RNP_LOG("Too few bytes for valid KBX");
            return false;
        }
        while (has_bytes > 4) {
            size_t blob_length = read_uint32(buf);
            if (blob_length > BLOB_SIZE_LIMIT) {
                RNP_LOG("Blob size is %zu bytes but limit is %d bytes",
                        blob_length,
                        (int) BLOB_SIZE_LIMIT);
                return false;
            }
            if (blob_length < BLOB_HEADER_SIZE) {
                RNP_LOG("Too small blob header size");
                return false;
            }
            if (has_bytes < blob_length) {
                RNP_LOG("Blob have size %zu bytes but file contains only %zu bytes",
                        blob_length,
                        has_bytes);
                return false;
            }
            auto blob = kbx_parse_blob(buf, blob_length);
            if (!blob.get()) {
                RNP_LOG("Failed to parse blob");
                return false;
            }
            kbx_blob_t *pblob = blob.get();
            blobs.push_back(std::move(blob));

            if (pblob->type() == KBX_PGP_BLOB) {
                // parse keyblock if it existed
                kbx_pgp_blob_t &pgp_blob = dynamic_cast<kbx_pgp_blob_t &>(*pblob);
                if (!pgp_blob.keyblock_length()) {
                    RNP_LOG("PGP blob have zero size");
                    return false;
                }

                MemorySource blsrc(pgp_blob.image().data() + pgp_blob.keyblock_offset(),
                                   pgp_blob.keyblock_length(),
                                   false);
                if (load_pgp(blsrc.src())) {
                    return false;
                }
            }

            has_bytes -= blob_length;
            buf += blob_length;
        }
        if (has_bytes) {
            RNP_LOG("KBX source has excess trailing bytes");
        }
        return true;
    } catch (const std::exception &e) {
        /* LCOV_EXCL_START */
        RNP_LOG("%s", e.what());
        return false;
        /* LCOV_EXCL_END */
    }
}

namespace {
bool
pbuf(pgp_dest_t &dst, const void *buf, size_t len)
{
    dst_write(&dst, buf, len);
    return dst.werr == RNP_SUCCESS;
}

bool
pu8(pgp_dest_t &dst, uint8_t p)
{
    return pbuf(dst, &p, 1);
}

bool
pu16(pgp_dest_t &dst, uint16_t f)
{
    uint8_t p[2];
    p[0] = (uint8_t)(f >> 8);
    p[1] = (uint8_t) f;
    return pbuf(dst, p, 2);
}

bool
pu32(pgp_dest_t &dst, uint32_t f)
{
    uint8_t p[4];
    write_uint32(p, f);
    return pbuf(dst, p, 4);
}

bool
kbx_write_header(const KeyStore &key_store, pgp_dest_t &dst)
{
    uint16_t flags = 0;
    uint32_t file_created_at = key_store.secctx.time();

    if (!key_store.blobs.empty() && (key_store.blobs[0]->type() == KBX_HEADER_BLOB)) {
        kbx_header_blob_t &blob = dynamic_cast<kbx_header_blob_t &>(*key_store.blobs[0]);
        file_created_at = blob.file_created_at();
    }

    return !(!pu32(dst, BLOB_FIRST_SIZE) || !pu8(dst, KBX_HEADER_BLOB) ||
             !pu8(dst, 1)                                                   // version
             || !pu16(dst, flags) || !pbuf(dst, "KBXf", 4) || !pu32(dst, 0) // RFU
             || !pu32(dst, 0)                                               // RFU
             || !pu32(dst, file_created_at) || !pu32(dst, key_store.secctx.time()) ||
             !pu32(dst, 0)); // RFU
}

bool
kbx_write_pgp(const KeyStore &key_store, const Key &key, pgp_dest_t &dst)
{
    MemoryDest mem(NULL, BLOB_SIZE_LIMIT);

    if (!pu32(mem.dst(), 0)) { // length, we don't know length of blob yet, so it's 0
        return false;
    }

    if (!pu8(mem.dst(), KBX_PGP_BLOB) || !pu8(mem.dst(), 1)) { // type, version
        return false;
    }

    if (!pu16(mem.dst(), 0)) { // flags, not used by GnuPG
        return false;
    }

    if (!pu32(mem.dst(), 0) ||
        !pu32(mem.dst(), 0)) { // offset and length of keyblock, update later
        return false;
    }

    if (!pu16(mem.dst(), 1 + key.subkey_count())) { // number of keys in keyblock
        return false;
    }
    if (!pu16(mem.dst(), 28)) { // size of key info structure)
        return false;
    }

    if (!pbuf(mem.dst(), key.fp().data(), key.fp().size()) ||
        !pu32(mem.dst(), mem.writeb() - 8) || // offset to keyid (part of fpr for V4)
        !pu16(mem.dst(), 0) ||                // flags, not used by GnuPG
        !pu16(mem.dst(), 0)) {                // RFU
        return false;
    }

    // same as above, for each subkey
    std::vector<uint32_t> subkey_sig_expirations;
    for (auto &sfp : key.subkey_fps()) {
        auto *subkey = key_store.get_key(sfp);
        if (!subkey || !pbuf(mem.dst(), subkey->fp().data(), subkey->fp().size()) ||
            !pu32(mem.dst(), mem.writeb() - 8) || // offset to keyid (part of fpr for V4)
            !pu16(mem.dst(), 0) ||                // flags, not used by GnuPG
            !pu16(mem.dst(), 0)) {                // RFU
            return false;
        }
        // load signature expirations while we're at it
        for (size_t i = 0; i < subkey->sig_count(); i++) {
            uint32_t expiration = subkey->get_sig(i).sig.key_expiration();
            subkey_sig_expirations.push_back(expiration);
        }
    }

    if (!pu16(mem.dst(), 0)) { // Zero size of serial number
        return false;
    }

    // skip serial number
    if (!pu16(mem.dst(), key.uid_count()) || !pu16(mem.dst(), 12)) {
        return false;
    }

    size_t uid_start = mem.writeb();
    for (size_t i = 0; i < key.uid_count(); i++) {
        if (!pu32(mem.dst(), 0) ||
            !pu32(mem.dst(), 0)) { // UID offset and length, update when blob has done
            return false;
        }

        if (!pu16(mem.dst(), 0)) { // flags, (not yet used)
            return false;
        }

        if (!pu8(mem.dst(), 0) || !pu8(mem.dst(), 0)) { // Validity & RFU
            return false;
        }
    }

    if (!pu16(mem.dst(), key.sig_count() + subkey_sig_expirations.size()) ||
        !pu16(mem.dst(), 4)) {
        return false;
    }

    for (size_t i = 0; i < key.sig_count(); i++) {
        if (!pu32(mem.dst(), key.get_sig(i).sig.key_expiration())) {
            return false;
        }
    }
    for (auto &expiration : subkey_sig_expirations) {
        if (!pu32(mem.dst(), expiration)) {
            return false;
        }
    }

    if (!pu8(mem.dst(), 0) ||
        !pu8(mem.dst(), 0)) { // Assigned ownertrust & All_Validity (not yet used)
        return false;
    }

    if (!pu16(mem.dst(), 0) || !pu32(mem.dst(), 0)) { // RFU & Recheck_after
        return false;
    }

    if (!pu32(mem.dst(), key_store.secctx.time()) ||
        !pu32(mem.dst(), key_store.secctx.time())) { // Latest timestamp && created
        return false;
    }

    if (!pu32(mem.dst(), 0)) { // Size of reserved space
        return false;
    }

    // wrtite UID, we might redesign PGP write and use this information from keyblob
    for (size_t i = 0; i < key.uid_count(); i++) {
        const auto &uid = key.get_uid(i);
        uint8_t *   p = (uint8_t *) mem.memory() + uid_start + (12 * i);
        /* store absolute uid offset in the output stream */
        uint32_t pt = mem.writeb() + dst.writeb;
        write_uint32(p, pt);
        /* and uid length */
        pt = uid.str.size();
        write_uint32(p + 4, pt);
        /* uid data itself */
        if (!pbuf(mem.dst(), uid.str.c_str(), pt)) {
            return false;
        }
    }

    /* write keyblock and fix the offset/length */
    size_t   key_start = mem.writeb();
    uint32_t pt = key_start;
    uint8_t *p = (uint8_t *) mem.memory() + 8;
    write_uint32(p, pt);

    key.write(mem.dst());
    if (mem.werr()) {
        return false;
    }

    for (auto &sfp : key.subkey_fps()) {
        auto *subkey = key_store.get_key(sfp);
        if (!subkey) {
            return false;
        }
        subkey->write(mem.dst());
        if (mem.werr()) {
            return false;
        }
    }

    /* key blob length */
    pt = mem.writeb() - key_start;
    p = (uint8_t *) mem.memory() + 12;
    write_uint32(p, pt);

    // fix the length of blob
    pt = mem.writeb() + 20;
    p = (uint8_t *) mem.memory();
    write_uint32(p, pt);

    // checksum
    auto hash = rnp::Hash::create(PGP_HASH_SHA1);
    hash->add(mem.memory(), mem.writeb());
    uint8_t checksum[PGP_SHA1_HASH_SIZE];
    assert(hash->size() == sizeof(checksum));
    hash->finish(checksum);

    if (!(pbuf(mem.dst(), checksum, PGP_SHA1_HASH_SIZE))) {
        return false;
    }

    /* finally write to the output */
    dst_write(&dst, mem.memory(), mem.writeb());
    return !dst.werr;
}

bool
kbx_write_x509(const KeyStore &key_store, pgp_dest_t &dst)
{
    for (auto &blob : key_store.blobs) {
        if (blob->type() != KBX_X509_BLOB) {
            continue;
        }
        if (!pbuf(dst, blob->image().data(), blob->length())) {
            return false;
        }
    }
    return true;
}
} // namespace

bool
KeyStore::write_kbx(pgp_dest_t &dst)
{
    try {
        if (!kbx_write_header(*this, dst)) {
            RNP_LOG("Can't write KBX header");
            return false;
        }

        for (auto &key : keys) {
            if (!key.is_primary()) {
                continue;
            }
            if (!kbx_write_pgp(*this, key, dst)) {
                RNP_LOG("Can't write PGP blobs for key %p", &key);
                return false;
            }
        }

        if (!kbx_write_x509(*this, dst)) {
            RNP_LOG("Can't write X509 blobs");
            return false;
        }
        return true;
    } catch (const std::exception &e) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to write KBX store: %s", e.what());
        return false;
        /* LCOV_EXCL_END */
    }
}
} // namespace rnp
