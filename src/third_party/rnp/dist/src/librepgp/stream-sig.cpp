/*
 * Copyright (c) 2018-2025, [Ribose Inc](https://www.ribose.com).
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
#include <type_traits>
#include <stdexcept>
#include <cinttypes>
#include <cassert>
#include <rnp/rnp_def.h>
#include "types.h"
#include "stream-sig.h"
#include "stream-packet.h"
#include "stream-armor.h"
#include "key.hpp"
#include "crypto/signatures.h"
#include <cassert>

#include <time.h>

void
signature_hash_key(const pgp_key_pkt_t &key, rnp::Hash &hash, pgp_version_t pgpver)
{
    if (key.pub_data.empty()) {
        /* call self recursively if hashed data is not filled, to overcome const restriction */
        pgp_key_pkt_t keycp(key, true);
        keycp.fill_hashed_data();
        signature_hash_key(keycp, hash, pgpver);
        return;
    }

    switch (pgpver) {
    case PGP_V2:
        FALLTHROUGH_STATEMENT;
    case PGP_V3:
        FALLTHROUGH_STATEMENT;
    case PGP_V4: {
        assert(key.pub_data.size() <= ((size_t) 0xffffu));
        uint8_t hdr[3] = {0x99, 0x00, 0x00};
        write_uint16(hdr + 1, key.pub_data.size());
        hash.add(hdr, 3);
        hash.add(key.pub_data);
        break;
    }
    case PGP_V5: {
        assert(key.pub_data.size() <= (size_t) 0xffffffffu);
        uint8_t hdr[5] = {0x9A, 0x00, 0x00, 0x00, 0x00};
        write_uint32(hdr + 1, key.pub_data.size());
        hash.add(&hdr, 5);
        hash.add(key.pub_data);
        break;
    }
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_V6: {
        assert(key.pub_data.size() <= (size_t) 0xffffffffu);
        uint8_t hdr[5] = {0x9b, 0x00, 0x00, 0x00, 0x00};
        write_uint32(hdr + 1, key.pub_data.size());
        hash.add(hdr, sizeof(hdr));
        hash.add(key.pub_data);
        break;
    }
#endif
    default:
        RNP_LOG("unknown key/sig version: %d", (int) pgpver);
        throw rnp::rnp_exception(RNP_ERROR_OUT_OF_MEMORY);
    }
}

void
signature_hash_userid(const pgp_userid_pkt_t &uid, rnp::Hash &hash, pgp_version_t sigver)
{
    if (sigver < PGP_V4) {
        hash.add(uid.uid.data(), uid.uid.size());
        return;
    }

    uint8_t hdr[5] = {0};
    switch (uid.tag) {
    case PGP_PKT_USER_ID:
        hdr[0] = 0xB4;
        break;
    case PGP_PKT_USER_ATTR:
        hdr[0] = 0xD1;
        break;
    default:
        RNP_LOG("wrong uid");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
    write_uint32(hdr + 1, uid.uid.size());
    hash.add(hdr, 5);
    hash.add(uid.uid.data(), uid.uid.size());
}

std::unique_ptr<rnp::Hash>
signature_hash_certification(const pgp::pkt::Signature &sig,
                             const pgp_key_pkt_t &      key,
                             const pgp_userid_pkt_t &   userid)
{
    auto hash = signature_init(key, sig);
    signature_hash_key(key, *hash, sig.version);
    signature_hash_userid(userid, *hash, sig.version);
    return hash;
}

std::unique_ptr<rnp::Hash>
signature_hash_binding(const pgp::pkt::Signature &sig,
                       const pgp_key_pkt_t &      key,
                       const pgp_key_pkt_t &      subkey)
{
    auto hash = signature_init(key, sig);
    signature_hash_key(key, *hash, sig.version);
    signature_hash_key(subkey, *hash, sig.version);
    return hash;
}

std::unique_ptr<rnp::Hash>
signature_hash_direct(const pgp::pkt::Signature &sig, const pgp_key_pkt_t &key)
{
    auto hash = signature_init(key, sig);
    signature_hash_key(key, *hash, sig.version);
    return hash;
}

rnp_result_t
process_pgp_signatures(pgp_source_t &src, pgp::pkt::Signatures &sigs)
{
    sigs.clear();
    /* Allow binary or armored input, including multiple armored messages */
    rnp::ArmoredSource armor(
      src, rnp::ArmoredSource::AllowBinary | rnp::ArmoredSource::AllowMultiple);
    /* read sequence of OpenPGP signatures */
    while (!armor.error()) {
        if (armor.eof() && armor.multiple()) {
            armor.restart();
        }
        if (armor.eof()) {
            break;
        }
        int ptag = stream_pkt_type(armor.src());
        if (ptag != PGP_PKT_SIGNATURE) {
            RNP_LOG("wrong signature tag: %d", ptag);
            sigs.clear();
            return RNP_ERROR_BAD_FORMAT;
        }

        sigs.emplace_back();
        rnp_result_t ret = sigs.back().parse(armor.src());
        if (ret) {
            sigs.clear();
            return ret;
        }
    }
    if (armor.error()) {
        sigs.clear();
        return RNP_ERROR_READ;
    }
    return RNP_SUCCESS;
}

namespace pgp {
namespace pkt {

bool
Signature::operator==(const Signature &src) const
{
    // TODO-V6: could also compare salt
    return (lbits == src.lbits) && (hashed_data == src.hashed_data) &&
           (material_buf == src.material_buf);
}

bool
Signature::operator!=(const Signature &src) const
{
    return !(*this == src);
}

SigID
Signature::get_id() const
{
    auto hash = rnp::Hash::create(PGP_HASH_SHA1);
    hash->add(hashed_data);
    hash->add(material_buf);
    SigID res = {0};
    static_assert(std::tuple_size<decltype(res)>::value == PGP_SHA1_HASH_SIZE,
                  "SigID size mismatch");
    hash->finish(res.data());
    return res;
}

sigsub::Raw *
Signature::get_subpkt(uint8_t stype, bool hashed)
{
    size_t idx = find_subpkt(stype, hashed);
    return idx == SIZE_MAX ? nullptr : subpkts[idx].get();
}

const sigsub::Raw *
Signature::get_subpkt(uint8_t stype, bool hashed) const
{
    size_t idx = find_subpkt(stype, hashed);
    return idx == SIZE_MAX ? nullptr : subpkts[idx].get();
}

sigsub::Raw *
Signature::get_subpkt(sigsub::Type type, bool hashed)
{
    return get_subpkt(static_cast<uint8_t>(type), hashed);
}

const sigsub::Raw *
Signature::get_subpkt(sigsub::Type type, bool hashed) const
{
    return get_subpkt(static_cast<uint8_t>(type), hashed);
}

bool
Signature::has_subpkt(uint8_t stype, bool hashed) const
{
    return find_subpkt(stype, hashed) != SIZE_MAX;
}

bool
Signature::has_keyid() const
{
    return (version < PGP_V4) || has_subpkt(PGP_SIG_SUBPKT_ISSUER_KEY_ID, false) ||
           has_keyfp();
}

KeyID
Signature::keyid() const noexcept
{
    /* version 3 uses signature field */
    if (version < PGP_V4) {
        return signer;
    }

    /* version 4 and up use subpackets */
    if (version == PGP_V4) {
        auto sub = dynamic_cast<const sigsub::IssuerKeyID *>(
          get_subpkt(sigsub::Type::IssuerKeyID, false));
        if (sub) {
            return sub->keyid();
        }
    }
    /* v5 and up must have fingerprint, from which keyid would be extracted */
    return keyfp().keyid();
}

void
Signature::set_keyid(const KeyID &id)
{
    if (version < PGP_V4) {
        signer = id;
        return;
    }

    auto sub = std::unique_ptr<sigsub::IssuerKeyID>(new sigsub::IssuerKeyID(false));
    sub->set_keyid(id);
    add_subpkt(std::move(sub));
}

bool
Signature::has_keyfp() const
{
    auto sub = dynamic_cast<const sigsub::IssuerFingerprint *>(
      get_subpkt(sigsub::Type::IssuerFingerprint));
    if (!sub) {
        return false;
    }
    switch (version) {
    case PGP_V4:
        return sub->fp().size() == PGP_FINGERPRINT_V4_SIZE;
    case PGP_V5:
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_V6:
#endif
        return sub->fp().size() == PGP_FINGERPRINT_V5_SIZE;
    default:
        return false;
    }
}

Fingerprint
Signature::keyfp() const noexcept
{
    auto sub = dynamic_cast<const sigsub::IssuerFingerprint *>(
      get_subpkt(sigsub::Type::IssuerFingerprint));
    return sub ? sub->fp() : Fingerprint{};
}

void
Signature::set_keyfp(const Fingerprint &fp)
{
    auto sub = std::unique_ptr<sigsub::IssuerFingerprint>(new sigsub::IssuerFingerprint());
#if defined(ENABLE_CRYPTO_REFRESH)
    sub->set_version(version);
#else
    sub->set_version(4);
#endif
    sub->set_fp(fp);
    add_subpkt(std::move(sub));
}

uint32_t
Signature::creation() const
{
    if (version < PGP_V4) {
        return creation_time;
    }
    auto sub =
      dynamic_cast<const sigsub::CreationTime *>(get_subpkt(sigsub::Type::CreationTime));
    return sub ? sub->time() : 0;
}

void
Signature::set_creation(uint32_t ctime)
{
    if (version < PGP_V4) {
        creation_time = ctime;
        return;
    }
    auto sub = std::unique_ptr<sigsub::CreationTime>(new sigsub::CreationTime());
    sub->set_time(ctime);
    add_subpkt(std::move(sub));
}

uint32_t
Signature::expiration() const
{
    auto sub =
      dynamic_cast<const sigsub::ExpirationTime *>(get_subpkt(sigsub::Type::ExpirationTime));
    return sub ? sub->time() : 0;
}

void
Signature::set_expiration(uint32_t etime)
{
    auto sub = std::unique_ptr<sigsub::ExpirationTime>(new sigsub::ExpirationTime());
    sub->set_time(etime);
    add_subpkt(std::move(sub));
}

uint32_t
Signature::key_expiration() const
{
    auto sub = dynamic_cast<const sigsub::KeyExpirationTime *>(
      get_subpkt(sigsub::Type::KeyExpirationTime));
    return sub ? sub->time() : 0;
}

void
Signature::set_key_expiration(uint32_t etime)
{
    auto sub = std::unique_ptr<sigsub::KeyExpirationTime>(new sigsub::KeyExpirationTime());
    sub->set_time(etime);
    add_subpkt(std::move(sub));
}

uint8_t
Signature::key_flags() const
{
    auto sub = dynamic_cast<const sigsub::KeyFlags *>(get_subpkt(sigsub::Type::KeyFlags));
    return sub ? sub->flags() : 0;
}

void
Signature::set_key_flags(uint8_t flags)
{
    auto sub = std::unique_ptr<sigsub::KeyFlags>(new sigsub::KeyFlags());
    sub->set_flags(flags);
    add_subpkt(std::move(sub));
}

bool
Signature::primary_uid() const
{
    auto sub =
      dynamic_cast<const sigsub::PrimaryUserID *>(get_subpkt(sigsub::Type::PrimaryUserID));
    return sub ? sub->primary() : 0;
}

void
Signature::set_primary_uid(bool primary)
{
    auto sub = std::unique_ptr<sigsub::PrimaryUserID>(new sigsub::PrimaryUserID());
    sub->set_primary(primary);
    add_subpkt(std::move(sub));
}

std::vector<uint8_t>
Signature::preferred(sigsub::Type type) const
{
    auto sub = dynamic_cast<const sigsub::Preferred *>(get_subpkt(type));
    return sub ? sub->algs() : std::vector<uint8_t>();
}

void
Signature::set_preferred(const std::vector<uint8_t> &data, sigsub::Type type)
{
    if (data.empty()) {
        /* Here we assume that there could be only one subpacket of the corresponding type */
        remove_subpkt(find_subpkt(type));
        remove_subpkt(find_subpkt(type, false));
        return;
    }

    auto sub = sigsub::Raw::create(type);
    auto pref = dynamic_cast<sigsub::Preferred *>(sub.get());
    if (!pref) {
        return;
    }
    pref->set_algs(data);
    add_subpkt(std::move(sub));
}

std::vector<uint8_t>
Signature::preferred_symm_algs() const
{
    return preferred(sigsub::Type::PreferredSymmetric);
}

void
Signature::set_preferred_symm_algs(const std::vector<uint8_t> &algs)
{
    set_preferred(algs, sigsub::Type::PreferredSymmetric);
}

std::vector<uint8_t>
Signature::preferred_hash_algs() const
{
    return preferred(sigsub::Type::PreferredHash);
}

void
Signature::set_preferred_hash_algs(const std::vector<uint8_t> &algs)
{
    set_preferred(algs, sigsub::Type::PreferredHash);
}

std::vector<uint8_t>
Signature::preferred_z_algs() const
{
    return preferred(sigsub::Type::PreferredCompress);
}

void
Signature::set_preferred_z_algs(const std::vector<uint8_t> &algs)
{
    set_preferred(algs, sigsub::Type::PreferredCompress);
}

#if defined(ENABLE_CRYPTO_REFRESH)
void
Signature::set_preferred_aead_algs(const std::vector<uint8_t> &algs)
{
    set_preferred(algs, sigsub::Type::PreferredAEADv6);
}

std::vector<uint8_t>
Signature::preferred_aead_algs() const
{
    return preferred(sigsub::Type::PreferredAEADv6);
}
#endif

uint8_t
Signature::key_server_prefs() const
{
    auto sub =
      dynamic_cast<const sigsub::KeyserverPrefs *>(get_subpkt(sigsub::Type::KeyserverPrefs));
    return sub ? sub->raw() : 0;
}

void
Signature::set_key_server_prefs(uint8_t prefs)
{
    auto sub = std::unique_ptr<sigsub::KeyserverPrefs>(new sigsub::KeyserverPrefs());
    sub->set_raw(prefs);
    add_subpkt(std::move(sub));
}

std::string
Signature::key_server() const
{
    auto sub = dynamic_cast<const sigsub::PreferredKeyserver *>(
      get_subpkt(sigsub::Type::PreferredKeyserver));
    return sub ? sub->keyserver() : "";
}

void
Signature::set_key_server(const std::string &uri)
{
    if (uri.empty()) {
        remove_subpkt(find_subpkt(sigsub::Type::PreferredKeyserver));
        remove_subpkt(find_subpkt(sigsub::Type::PreferredKeyserver, false));
        return;
    }

    auto sub = std::unique_ptr<sigsub::PreferredKeyserver>(new sigsub::PreferredKeyserver());
    sub->set_keyserver(uri);
    add_subpkt(std::move(sub));
}

uint8_t
Signature::trust_level() const
{
    auto sub = dynamic_cast<const sigsub::Trust *>(get_subpkt(sigsub::Type::Trust));
    return sub ? sub->level() : 0;
}

uint8_t
Signature::trust_amount() const
{
    auto sub = dynamic_cast<const sigsub::Trust *>(get_subpkt(sigsub::Type::Trust));
    return sub ? sub->amount() : 0;
}

void
Signature::set_trust(uint8_t level, uint8_t amount)
{
    auto sub = std::unique_ptr<sigsub::Trust>(new sigsub::Trust());
    sub->set_level(level);
    sub->set_amount(amount);
    add_subpkt(std::move(sub));
}

bool
Signature::revocable() const
{
    auto sub = dynamic_cast<const sigsub::Revocable *>(get_subpkt(sigsub::Type::Revocable));
    return sub ? sub->revocable() : true;
}

void
Signature::set_revocable(bool status)
{
    auto sub = std::unique_ptr<sigsub::Revocable>(new sigsub::Revocable());
    sub->set_revocable(status);
    add_subpkt(std::move(sub));
}

std::string
Signature::revocation_reason() const
{
    auto sub = dynamic_cast<const sigsub::RevocationReason *>(
      get_subpkt(sigsub::Type::RevocationReason));
    return sub ? sub->reason() : "";
}

pgp_revocation_type_t
Signature::revocation_code() const
{
    auto sub = dynamic_cast<const sigsub::RevocationReason *>(
      get_subpkt(sigsub::Type::RevocationReason));
    return sub ? sub->code() : PGP_REVOCATION_NO_REASON;
}

bool
Signature::has_revocation_reason() const
{
    return get_subpkt(sigsub::Type::RevocationReason);
}

void
Signature::set_revocation_reason(pgp_revocation_type_t code, const std::string &reason)
{
    auto sub = std::unique_ptr<sigsub::RevocationReason>(new sigsub::RevocationReason());
    sub->set_code(code);
    sub->set_reason(reason);
    add_subpkt(std::move(sub));
}

uint32_t
Signature::key_get_features() const
{
    auto sub = dynamic_cast<const sigsub::Features *>(get_subpkt(sigsub::Type::Features));
    return sub ? sub->features() : 0;
}

bool
Signature::key_has_features(uint32_t flags) const
{
    auto sub = dynamic_cast<const sigsub::Features *>(get_subpkt(sigsub::Type::Features));
    return sub ? sub->features() & flags : false;
}

void
Signature::set_key_features(uint32_t flags)
{
    auto sub = std::unique_ptr<sigsub::Features>(new sigsub::Features());
    sub->set_features(flags & 0xff);
    add_subpkt(std::move(sub));
}

std::string
Signature::signer_uid() const
{
    auto sub =
      dynamic_cast<const sigsub::SignersUserID *>(get_subpkt(sigsub::Type::SignersUserID));
    return sub ? sub->signer() : "";
}

void
Signature::set_signer_uid(const std::string &uid)
{
    auto sub = std::unique_ptr<sigsub::SignersUserID>(new sigsub::SignersUserID());
    sub->set_signer(uid);
    add_subpkt(std::move(sub));
}

void
Signature::add_notation(const std::string &         name,
                        const std::vector<uint8_t> &value,
                        bool                        human,
                        bool                        critical)
{
    if ((name.size() > 0xffff) || (value.size() > 0xffff)) {
        RNP_LOG("wrong length");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }

    auto sub = std::unique_ptr<sigsub::NotationData>(new sigsub::NotationData(true, critical));
    sub->set_human_readable(human);
    sub->set_name(name);
    sub->set_value(value);
    add_subpkt(std::move(sub), false);
}

void
Signature::add_notation(const std::string &name, const std::string &value, bool critical)
{
    add_notation(name, std::vector<uint8_t>(value.begin(), value.end()), true, critical);
}

void
Signature::set_embedded_sig(const Signature &esig)
{
    auto sub =
      std::unique_ptr<sigsub::EmbeddedSignature>(new sigsub::EmbeddedSignature(false));
    sub->set_signature(esig);
    add_subpkt(std::move(sub));
}

const sigsub::RevocationKey *
Signature::revoker_subpkt() const noexcept
{
    return dynamic_cast<const sigsub::RevocationKey *>(
      get_subpkt(sigsub::Type::RevocationKey));
}

bool
Signature::has_revoker() const noexcept
{
    return revoker_subpkt();
}

Fingerprint
Signature::revoker() const noexcept
{
    auto sub = revoker_subpkt();
    return sub ? sub->fp() : Fingerprint();
}

void
Signature::set_revoker(const rnp::Key &revoker, bool sensitive)
{
    auto sub = std::unique_ptr<sigsub::RevocationKey>(new sigsub::RevocationKey());
    sub->set_rev_class(sensitive ? 0xC0 : 0x80);
    sub->set_alg(revoker.alg());
    sub->set_fp(revoker.fp());
    add_subpkt(std::move(sub));
}

void
Signature::add_subpkt(std::unique_ptr<sigsub::Raw> &&sub, bool replace)
{
    if (version < PGP_V4) {
        RNP_LOG("wrong signature version");
        throw std::invalid_argument("version");
    }

    sub->write();
    if (replace) {
        auto idx = find_subpkt(sub->raw_type(), sub->hashed());
        if (idx != SIZE_MAX) {
            subpkts[idx] = std::move(sub);
            return;
        }
    }
    subpkts.items.push_back(std::move(sub));
}

void
Signature::remove_subpkt(size_t idx)
{
    if (idx < subpkts.size()) {
        subpkts.items.erase(subpkts.begin() + idx);
    }
}

bool
Signature::matches_onepass(const pgp_one_pass_sig_t &onepass) const
{
    if (!has_keyid()) {
        return false;
    }
    return (halg == onepass.halg) && (palg == onepass.palg) && (type_ == onepass.type) &&
           (onepass.keyid == keyid());
}

bool
Signature::version_supported(pgp_version_t version)
{
    if ((version >= PGP_V2) && (version <= PGP_V5)) {
        return true;
    }
#if defined(ENABLE_CRYPTO_REFRESH)
    return version == PGP_V6;
#else
    return false;
#endif
}

rnp_result_t
Signature::parse_v2v3(pgp_packet_body_t &pkt)
{
    /* parse v2/v3-specific fields, not the whole signature */
    uint8_t buf[16] = {};
    if (!pkt.get(buf, 16)) {
        RNP_LOG("cannot get enough bytes");
        return RNP_ERROR_BAD_FORMAT;
    }
    /* length of hashed data, 5 */
    if (buf[0] != 5) {
        RNP_LOG("wrong length of hashed data");
        return RNP_ERROR_BAD_FORMAT;
    }
    /* hashed data */
    hashed_data.assign(buf + 1, buf + 6);
    /* signature type */
    type_ = (pgp_sig_type_t) buf[1];
    /* creation time */
    creation_time = read_uint32(&buf[2]);
    /* signer's key id */
    static_assert(std::tuple_size<decltype(signer)>::value == PGP_KEY_ID_SIZE,
                  "v3 signer field size mismatch");
    memcpy(signer.data(), &buf[6], PGP_KEY_ID_SIZE);
    /* public key algorithm */
    palg = (pgp_pubkey_alg_t) buf[14];
    /* hash algorithm */
    halg = (pgp_hash_alg_t) buf[15];
    return RNP_SUCCESS;
}

#define MAX_SUBPACKETS 64

bool
Signature::parse_subpackets(uint8_t *buf, size_t len, bool hashed)
{
    bool res = true;

    while (len) {
        if (subpkts.size() >= MAX_SUBPACKETS) {
            RNP_LOG("too many signature subpackets");
            return false;
        }
        if (len < 2) {
            RNP_LOG("got single byte %" PRIu8, *buf);
            return false;
        }

        /* subpacket length */
        size_t splen = *buf++;
        len--;
        if ((splen >= 192) && (splen < 255)) {
            splen = ((splen - 192) << 8) + *buf++ + 192;
            len--;
        } else if (splen == 255) {
            if (len < 4) {
                RNP_LOG("got 4-byte len but only %zu bytes in buffer", len);
                return false;
            }
            splen = read_uint32(buf);
            buf += 4;
            len -= 4;
        }

        if (!splen) {
            RNP_LOG("got subpacket with 0 length");
            return false;
        }

        /* subpacket data */
        if (len < splen) {
            RNP_LOG("got subpacket len %zu, while only %zu bytes left", splen, len);
            return false;
        }

        auto subpkt = sigsub::Raw::create(buf, splen, hashed);
        if (!subpkt) {
            res = false;
        } else {
            subpkts.items.push_back(std::move(subpkt));
        }
        len -= splen;
        buf += splen;
    }
    return res;
}

bool
Signature::get_subpkt_len(pgp_packet_body_t &pkt, size_t &splen)
{
    switch (version) {
    case PGP_V4:
    case PGP_V5: {
        uint16_t len = 0;
        if (!pkt.get(len)) {
            return false;
        }
        splen = len;
        return true;
    }
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_V6: {
        uint32_t len = 0;
        if (!pkt.get(len)) {
            return false;
        }
        splen = len;
        return true;
    }
#endif
    default:
        RNP_LOG("unsupported signature version: %d", (int) version);
        return false;
    }
}

size_t
Signature::find_subpkt(uint8_t stype, bool hashed, size_t skip) const
{
    if (version < PGP_V4) {
        return SIZE_MAX;
    }
    for (size_t idx = 0; idx < subpkts.size(); idx++) {
        if ((subpkts[idx]->raw_type() != stype) || (hashed && !subpkts[idx]->hashed())) {
            continue;
        }
        if (!skip) {
            return idx;
        }
        skip--;
    }
    return SIZE_MAX;
}

size_t
Signature::find_subpkt(sigsub::Type type, bool hashed, size_t skip) const
{
    return find_subpkt(static_cast<uint8_t>(type), hashed, skip);
}

rnp_result_t
Signature::parse_v4up(pgp_packet_body_t &pkt)
{
    /* parse v4 (and up) specific fields, not the whole signature */
    uint8_t buf[3];
    if (!pkt.get(buf, 3)) {
        RNP_LOG("cannot get first 3 bytes");
        return RNP_ERROR_BAD_FORMAT;
    }

    /* signature type */
    type_ = (pgp_sig_type_t) buf[0];
    /* public key algorithm */
    palg = (pgp_pubkey_alg_t) buf[1];
    /* hash algorithm */
    halg = (pgp_hash_alg_t) buf[2];
    /* hashed subpackets length */

    size_t splen = 0;
    auto   hash_begin = pkt.cur();
    if (!get_subpkt_len(pkt, splen)) {
        RNP_LOG("cannot get hashed len");
        return RNP_ERROR_BAD_FORMAT;
    }
    size_t splen_size = pkt.cur() - hash_begin;
    /* hashed subpackets length + splen_size bytes of length of unhashed subpackets */
    if (pkt.left() < splen + splen_size) {
        RNP_LOG("wrong packet or hashed subpackets length");
        return RNP_ERROR_BAD_FORMAT;
    }
    /* building hashed data */
    size_t hlen = 4 + splen + splen_size;
    hashed_data.resize(hlen);
    hashed_data[0] = version;
    static_assert(sizeof(buf) == 3, "Wrong signature header size.");
    pkt.skip_back(3 + splen_size);

    if (!pkt.get(hashed_data.data() + 1, hlen - 1)) {
        RNP_LOG("cannot get hashed subpackets data");
        return RNP_ERROR_BAD_FORMAT;
    }
    /* parsing hashed subpackets */
    if (!parse_subpackets(hashed_data.data() + 4 + splen_size, splen, true)) {
        RNP_LOG("failed to parse hashed subpackets");
        return RNP_ERROR_BAD_FORMAT;
    }

    /* reading unhashed subpackets */
    if (!get_subpkt_len(pkt, splen)) {
        RNP_LOG("cannot get unhashed len");
        return RNP_ERROR_BAD_FORMAT;
    }
    if (pkt.left() < splen) {
        RNP_LOG("not enough data for unhashed subpackets");
        return RNP_ERROR_BAD_FORMAT;
    }
    if (!parse_subpackets(pkt.cur(), splen, false)) {
        RNP_LOG("failed to parse unhashed subpackets");
        return RNP_ERROR_BAD_FORMAT;
    }
    pkt.skip(splen);
    return RNP_SUCCESS;
}

rnp_result_t
Signature::parse(pgp_packet_body_t &pkt)
{
    uint8_t ver = 0;
    if (!pkt.get(ver)) {
        return RNP_ERROR_BAD_FORMAT;
    }
    version = (pgp_version_t) ver;

    /* v3 or v4 or v6 signature body */
    rnp_result_t res;
    switch (ver) {
    case PGP_V2:
        FALLTHROUGH_STATEMENT;
    case PGP_V3:
        res = parse_v2v3(pkt);
        break;
    case PGP_V4:
        FALLTHROUGH_STATEMENT;
    case PGP_V5:
#if defined(ENABLE_CRYPTO_REFRESH)
        FALLTHROUGH_STATEMENT;
    case PGP_V6:
#endif
        res = parse_v4up(pkt);
        break;
    default:
        RNP_LOG("unknown signature version: %d", (int) ver);
        res = RNP_ERROR_BAD_FORMAT;
    }

    if (res) {
        return res;
    }

    /* left 16 bits of the hash */
    if (!pkt.get(lbits.data(), 2)) {
        RNP_LOG("not enough data for hash left bits");
        return RNP_ERROR_BAD_FORMAT;
    }

#if defined(ENABLE_CRYPTO_REFRESH)
    if (ver == PGP_V6) {
        uint8_t salt_size = 0;
        if (!pkt.get(salt_size)) {
            RNP_LOG("not enough data for v6 salt size octet");
            return RNP_ERROR_BAD_FORMAT;
        }
        if (salt_size != rnp::Hash::size(halg) / 2) {
            RNP_LOG("invalid salt size");
            return RNP_ERROR_BAD_FORMAT;
        }
        if (!pkt.get(salt, salt_size)) {
            RNP_LOG("not enough data for v6 signature salt");
            return RNP_ERROR_BAD_FORMAT;
        }
    }
#endif

    /* raw signature material */
    /* we cannot fail here */
    pkt.get(material_buf, pkt.left());
    /* check whether it can be parsed */
    if (!parse_material()) {
        return RNP_ERROR_BAD_FORMAT;
    }
    return RNP_SUCCESS;
}

rnp_result_t
Signature::parse(pgp_source_t &src)
{
    pgp_packet_body_t pkt(PGP_PKT_SIGNATURE);
    rnp_result_t      res = pkt.read(src);
    if (res) {
        return res;
    }
    return parse(pkt);
}

std::unique_ptr<SigMaterial>
Signature::parse_material() const
{
    auto sig = SigMaterial::create(palg, halg);
    if (!sig) {
        return nullptr;
    }
    pgp_packet_body_t pkt(material_buf);
    if (!sig->parse(pkt)) {
        return nullptr;
    }
    if (pkt.left()) {
        RNP_LOG("extra %zu bytes in pk packet", pkt.left());
        return nullptr;
    }
    return sig;
}

void
Signature::write(pgp_dest_t &dst, bool hdr) const
{
    if (!Signature::version_supported(version)) {
        RNP_LOG("don't know version %d", (int) version);
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }

    pgp_packet_body_t pktbody(PGP_PKT_SIGNATURE);

    if (version < PGP_V4) {
        /* for v3 signatures hashed data includes only type + creation_time */
        pktbody.add_byte(version);
        pktbody.add_byte(hashed_data.size());
        pktbody.add(hashed_data);
        pktbody.add(signer);
        pktbody.add_byte(palg);
        pktbody.add_byte(halg);
    } else {
        /* for v4 sig->hashed_data must contain most of signature fields */
        pktbody.add(hashed_data);
        pktbody.add_subpackets(*this, false);
    }
    pktbody.add(lbits.data(), 2);
#if defined(ENABLE_CRYPTO_REFRESH)
    if (version == PGP_V6) {
        pktbody.add_byte(salt.size());
        pktbody.add(salt);
    }
#endif
    /* write mpis */
    pktbody.add(material_buf);
    pktbody.write(dst, hdr);
}

std::vector<uint8_t>
Signature::write(bool hdr) const
{
    rnp::MemoryDest dst;
    write(dst.dst(), hdr);
    return dst.to_vector();
}

void
Signature::write_material(const SigMaterial &material)
{
    pgp_packet_body_t pktbody(PGP_PKT_SIGNATURE);
    material.write(pktbody);
    material_buf.assign(pktbody.data(), pktbody.data() + pktbody.size());
}

void
Signature::fill_hashed_data()
{
    /* we don't have a need to write v2-v3 signatures */
    if (!Signature::version_supported(version)) {
        RNP_LOG("don't know version %d", (int) version);
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
    pgp_packet_body_t hbody(PGP_PKT_RESERVED);
    if (version < PGP_V4) {
        hbody.add_byte(type());
        hbody.add_uint32(creation_time);
    } else {
        hbody.add_byte(version);
        hbody.add_byte(type());
        hbody.add_byte(palg);
        hbody.add_byte(halg);
        hbody.add_subpackets(*this, true);
    }
    hashed_data.assign(hbody.data(), hbody.data() + hbody.size());
}

} // namespace pkt
} // namespace pgp
