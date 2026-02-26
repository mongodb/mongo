/*
 * Copyright (c) 2024 [Ribose Inc](https://www.ribose.com).
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

#include "sig_subpacket.hpp"
#include "utils.h"
#include "logging.h"
#include "librepgp/stream-sig.h"
#include "key.hpp"
#include <cinttypes>
#include <cassert>

namespace pgp {

namespace pkt {

namespace sigsub {

/* Raw unparsed subpacket */
Raw::Raw(uint8_t rawtype, bool hashed, bool critical)
    : hashed_(hashed), raw_type_(rawtype), critical_(critical), type_(Type::Unknown)
{
}

Raw::Raw(Type type, bool hashed, bool critical)
    : hashed_(hashed), raw_type_(static_cast<uint8_t>(type)), critical_(critical), type_(type)
{
}

Raw::~Raw()
{
}

bool
Raw::check_size(size_t size) const noexcept
{
    return true;
};

bool
Raw::parse_data(const uint8_t *data, size_t size)
{
    return true;
};

bool
Raw::parse(const uint8_t *data, size_t size)
{
    if (!check_size(size)) {
        RNP_LOG("wrong len %zu of subpacket type %" PRIu8, size, raw_type_);
        return false;
    }
    if (!parse_data(data, size)) {
        return false;
    }
    data_.assign(data, data + size);
    return true;
}

const std::vector<uint8_t> &
Raw::write()
{
    if (data_.empty()) {
        write_data();
    }
    return data_;
}

static void
report_unknown(uint8_t type, bool critical)
{
    switch (type) {
    case (uint8_t) Type::Private_100:
    case (uint8_t) Type::Private_101:
    case (uint8_t) Type::Private_102:
    case (uint8_t) Type::Private_103:
    case (uint8_t) Type::Private_104:
    case (uint8_t) Type::Private_105:
    case (uint8_t) Type::Private_106:
    case (uint8_t) Type::Private_107:
    case (uint8_t) Type::Private_108:
    case (uint8_t) Type::Private_109:
    case (uint8_t) Type::Private_110:
        if (critical) {
            RNP_LOG("unknown critical private subpacket %" PRIu8, type);
        }
        return;
    case (uint8_t) Type::Reserved_1:
    case (uint8_t) Type::Reserved_8:
    case (uint8_t) Type::Placeholder:
    case (uint8_t) Type::Reserved_13:
    case (uint8_t) Type::Reserved_14:
    case (uint8_t) Type::Reserved_15:
    case (uint8_t) Type::Reserved_17:
    case (uint8_t) Type::Reserved_18:
    case (uint8_t) Type::Reserved_19:
        /* do not report reserved/placeholder subpacket */
        return;
    default:
        RNP_LOG("unknown subpacket : %" PRIu8, type);
    }
}

RawPtr
Raw::create(uint8_t type, bool hashed, bool critical)
{
    switch (type) {
    case (uint8_t) Type::CreationTime:
        return RawPtr(new CreationTime(hashed, critical));
    case (uint8_t) Type::ExpirationTime:
        return RawPtr(new ExpirationTime(hashed, critical));
    case (uint8_t) Type::ExportableCert:
        return RawPtr(new ExportableCert(hashed, critical));
    case (uint8_t) Type::Trust:
        return RawPtr(new Trust(hashed, critical));
    case (uint8_t) Type::RegExp:
        return RawPtr(new RegExp(hashed, critical));
    case (uint8_t) Type::Revocable:
        return RawPtr(new Revocable(hashed, critical));
    case (uint8_t) Type::KeyExpirationTime:
        return RawPtr(new KeyExpirationTime(hashed, critical));
    case (uint8_t) Type::PreferredSymmetric:
        return RawPtr(new PreferredSymmetric(hashed, critical));
    case (uint8_t) Type::RevocationKey:
        return RawPtr(new RevocationKey(hashed, critical));
    case (uint8_t) Type::IssuerKeyID:
        return RawPtr(new IssuerKeyID(hashed, critical));
    case (uint8_t) Type::NotationData:
        return RawPtr(new NotationData(hashed, critical));
    case (uint8_t) Type::PreferredHash:
        return RawPtr(new PreferredHash(hashed, critical));
    case (uint8_t) Type::PreferredCompress:
        return RawPtr(new PreferredCompress(hashed, critical));
    case (uint8_t) Type::KeyserverPrefs:
        return RawPtr(new KeyserverPrefs(hashed, critical));
    case (uint8_t) Type::PreferredKeyserver:
        return RawPtr(new PreferredKeyserver(hashed, critical));
    case (uint8_t) Type::PrimaryUserID:
        return RawPtr(new PrimaryUserID(hashed, critical));
    case (uint8_t) Type::PolicyURI:
        return RawPtr(new PolicyURI(hashed, critical));
    case (uint8_t) Type::KeyFlags:
        return RawPtr(new KeyFlags(hashed, critical));
    case (uint8_t) Type::SignersUserID:
        return RawPtr(new SignersUserID(hashed, critical));
    case (uint8_t) Type::RevocationReason:
        return RawPtr(new RevocationReason(hashed, critical));
    case (uint8_t) Type::Features:
        return RawPtr(new Features(hashed, critical));
    case (uint8_t) Type::EmbeddedSignature:
        return RawPtr(new EmbeddedSignature(hashed, critical));
    case (uint8_t) Type::IssuerFingerprint:
        return RawPtr(new IssuerFingerprint(hashed, critical));
    case (uint8_t) Type::PreferredAEAD:
        return RawPtr(new PreferredAEAD(hashed, critical));
#if defined(ENABLE_CRYPTO_REFRESH)
    case (uint8_t) Type::PreferredAEADv6:
        return RawPtr(new PreferredAEADv6(hashed, critical));
#endif
    default:
        report_unknown(type, critical);
        return RawPtr(new Raw(type, hashed, critical));
    }
}

RawPtr
Raw::create(Type type, bool hashed, bool critical)
{
    return create(static_cast<uint8_t>(type), hashed, critical);
}

RawPtr
Raw::create(const uint8_t *data, size_t size, bool hashed)
{
    if (!size) {
        RNP_LOG("got subpacket with 0 length");
        return nullptr;
    }
    bool    critical = data[0] & 0x80;
    uint8_t type = data[0] & 0x7f;
    auto    sub = create(type, hashed, critical);
    if (!sub || ((sub->type() == Type::Unknown) && critical) ||
        !sub->parse(data + 1, size - 1)) {
        return nullptr;
    }
    return sub;
}

RawPtr
Raw::clone() const
{
    return RawPtr(new Raw(*this));
}

/* Timestamp-based subpackets abstract parent */
void
Time::write_data()
{
    data_.resize(4);
    write_uint32(data_.data(), time_);
}

bool
Time::check_size(size_t size) const noexcept
{
    return size == 4;
}

bool
Time::parse_data(const uint8_t *data, size_t size)
{
    time_ = read_uint32(data);
    return true;
}

/* Creation time signature subpacket */
RawPtr
CreationTime::clone() const
{
    return RawPtr(new CreationTime(*this));
}

/* Signature expiration time signature subpacket */
RawPtr
ExpirationTime::clone() const
{
    return RawPtr(new ExpirationTime(*this));
}

/* Key expiration time signature subpacket */
RawPtr
KeyExpirationTime::clone() const
{
    return RawPtr(new KeyExpirationTime(*this));
}

/* Base class for subpackets with single bool value */
void
Bool::write_data()
{
    data_.resize(1);
    data_[0] = value_;
}

bool
Bool::check_size(size_t size) const noexcept
{
    return size == 1;
}

bool
Bool::parse_data(const uint8_t *data, size_t size)
{
    value_ = data[0];
    return true;
}

/* Exportable certification signature subpacket */
RawPtr
ExportableCert::clone() const
{
    return RawPtr(new ExportableCert(*this));
}

/* Trust signature subpacket */
void
Trust::write_data()
{
    data_.resize(2);
    data_[0] = level_;
    data_[1] = amount_;
}

bool
Trust::check_size(size_t size) const noexcept
{
    return size == 2;
}

bool
Trust::parse_data(const uint8_t *data, size_t size)
{
    level_ = data[0];
    amount_ = data[1];
    return true;
}

RawPtr
Trust::clone() const
{
    return RawPtr(new Trust(*this));
}

/* Base class for single string value subpackets */
void
String::write_data()
{
    data_.assign(value_.begin(), value_.end());
}

bool
String::parse_data(const uint8_t *data, size_t size)
{
    value_.assign(data, data + size);
    return true;
}

/* Regular expression signature subpacket */
RawPtr
RegExp::clone() const
{
    return RawPtr(new RegExp(*this));
}

/* Revocable signature subpacket */
RawPtr
Revocable::clone() const
{
    return RawPtr(new Revocable(*this));
}

/* Base class for preferred algorithms */
void
Preferred::write_data()
{
    data_ = algs_;
}

bool
Preferred::check_size(size_t size) const noexcept
{
    /* No reason to have more then 256 bytes */
    return size < 256;
}

bool
Preferred::parse_data(const uint8_t *data, size_t size)
{
    algs_.assign(data, data + size);
    return true;
}

/* Preferred symmetric algorithms */
RawPtr
PreferredSymmetric::clone() const
{
    return RawPtr(new PreferredSymmetric(*this));
}

/* Preferred hash algorithms */
RawPtr
PreferredHash::clone() const
{
    return RawPtr(new PreferredHash(*this));
}

/* Preferred compression algorithms */
RawPtr
PreferredCompress::clone() const
{
    return RawPtr(new PreferredCompress(*this));
}

/* Preferred AEAD algorithms for LibrePGP/pre-2021 crypto-refresh draft */
RawPtr
PreferredAEAD::clone() const
{
    return RawPtr(new PreferredAEAD(*this));
}

#if defined(ENABLE_CRYPTO_REFRESH)
/* Preferred AEAD algorithms as per RFC 9580 */
bool
PreferredAEADv6::check_size(size_t size) const noexcept
{
    if (size % 2) {
        RNP_LOG("v6 AEAD Ciphersuite Preferences must contain an even number of bytes");
        return false;
    }
    return Preferred::check_size(size);
}

RawPtr
PreferredAEADv6::clone() const
{
    return RawPtr(new PreferredAEADv6(*this));
}
#endif

/* Revocation key signature subpacket */
void
RevocationKey::write_data()
{
    data_.resize(2 + fp_.size());
    data_[0] = rev_class_;
    data_[1] = alg_;
    memcpy(data_.data() + 2, fp_.data(), fp_.size());
}

bool
RevocationKey::check_size(size_t size) const noexcept
{
    return (size == 2 + PGP_FINGERPRINT_V4_SIZE) || (size == 2 + PGP_FINGERPRINT_V5_SIZE);
}

bool
RevocationKey::parse_data(const uint8_t *data, size_t size)
{
    rev_class_ = data[0];
    alg_ = static_cast<pgp_pubkey_alg_t>(data[1]);
    fp_ = pgp::Fingerprint(data + 2, size - 2);
    return true;
}

RawPtr
RevocationKey::clone() const
{
    return RawPtr(new RevocationKey(*this));
}

/* Issuer Key ID signature subpacet */
void
IssuerKeyID::write_data()
{
    data_.assign(keyid_.begin(), keyid_.end());
}

bool
IssuerKeyID::check_size(size_t size) const noexcept
{
    return size == PGP_KEY_ID_SIZE;
}

bool
IssuerKeyID::parse_data(const uint8_t *data, size_t size)
{
    memcpy(keyid_.data(), data, keyid_.size());
    return true;
}

RawPtr
IssuerKeyID::clone() const
{
    return RawPtr(new IssuerKeyID(*this));
}

/* Notation data signature subpacket */
void
NotationData::write_data()
{
    assert(name_.size() <= 0xffff);
    assert(value_.size() <= 0xffff);
    uint16_t nlen = name_.size();
    uint16_t vlen = value_.size();
    data_.resize(8 + nlen + vlen);
    memcpy(data_.data(), flags_.data(), 4);
    write_uint16(data_.data() + 4, nlen);
    write_uint16(data_.data() + 6, vlen);
    memcpy(data_.data() + 8, name_.data(), nlen);
    memcpy(data_.data() + 8 + nlen, value_.data(), vlen);
}

bool
NotationData::check_size(size_t size) const noexcept
{
    return size >= 8;
}

bool
NotationData::parse_data(const uint8_t *data, size_t size)
{
    size_t nlen = read_uint16(data + 4);
    size_t vlen = read_uint16(data + 6);
    if (size != nlen + vlen + 8) {
        return false;
    }
    memcpy(flags_.data(), data, 4);
    name_.assign(data + 8, data + 8 + nlen);
    value_.assign(data + 8 + nlen, data + 8 + nlen + vlen);
    return true;
}

void
NotationData::set_human_readable(bool value) noexcept
{
    if (value) {
        flags_[0] |= 0x80;
    } else {
        flags_[0] &= ~0x80;
    }
}

void
NotationData::set_name(const std::string &name)
{
    if (name.size() > 0xffff) {
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
    name_ = name;
    data_.clear();
}

void
NotationData::set_value(const std::vector<uint8_t> &value)
{
    if (value.size() > 0xffff) {
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
    value_ = value;
    data_.clear();
}

RawPtr
NotationData::clone() const
{
    return RawPtr(new NotationData(*this));
}

/* Base class for bit flags subpackets */
void
Flags::write_data()
{
    data_.resize(1);
    data_[0] = flags_;
}

bool
Flags::check_size(size_t size) const noexcept
{
    return size >= 1;
}

bool
Flags::parse_data(const uint8_t *data, size_t size)
{
    flags_ = data[0];
    return true;
}

/* Key server preferences signature subpacket */
RawPtr
KeyserverPrefs::clone() const
{
    return RawPtr(new KeyserverPrefs(*this));
}

/* Preferred key server signature subpacket */
RawPtr
PreferredKeyserver::clone() const
{
    return RawPtr(new PreferredKeyserver(*this));
}

/* Primary userid signature subpacket */
RawPtr
PrimaryUserID::clone() const
{
    return RawPtr(new PrimaryUserID(*this));
}

/* Policy URI signature subpacket */
RawPtr
PolicyURI::clone() const
{
    return RawPtr(new PolicyURI(*this));
}

/* Key flags signature subpacket */
RawPtr
KeyFlags::clone() const
{
    return RawPtr(new KeyFlags(*this));
}

/* Signer's user ID signature subpacket */
RawPtr
SignersUserID::clone() const
{
    return RawPtr(new SignersUserID(*this));
}

/* Reason for revocation signature subpacket */
void
RevocationReason::write_data()
{
    data_.resize(1 + reason_.size());
    data_[0] = static_cast<uint8_t>(code_);
    memcpy(data_.data() + 1, reason_.data(), reason_.size());
}

bool
RevocationReason::check_size(size_t size) const noexcept
{
    return size >= 1;
}

bool
RevocationReason::parse_data(const uint8_t *data, size_t size)
{
    code_ = static_cast<pgp_revocation_type_t>(data[0]);
    reason_.assign(data + 1, data + size);
    return true;
}

RawPtr
RevocationReason::clone() const
{
    return RawPtr(new RevocationReason(*this));
}

/* Features signature subpacket */
RawPtr
Features::clone() const
{
    return RawPtr(new Features(*this));
}

/* Embedded signature signature subpacket */
EmbeddedSignature::EmbeddedSignature(const EmbeddedSignature &src) : Raw(src)
{
    signature_ = std::unique_ptr<Signature>(new Signature(*src.signature_));
}

EmbeddedSignature::EmbeddedSignature(bool hashed, bool critical)
    : Raw(Type::EmbeddedSignature, hashed, critical)
{
}

void
EmbeddedSignature::write_data()
{
    if (!signature_) {
        return;
    }
    data_ = signature_->write(false);
    if (data_.size() > 0xffff) {
        data_.clear();
        throw rnp::rnp_exception(RNP_ERROR_BAD_STATE);
    }
}

bool
EmbeddedSignature::check_size(size_t size) const noexcept
{
    return size > 6;
}

bool
EmbeddedSignature::parse_data(const uint8_t *data, size_t size)
{
    pgp_packet_body_t pkt(data, size);
    Signature         sig;
    if (sig.parse(pkt)) {
        return false;
    }
    signature_ = std::unique_ptr<Signature>(new Signature(std::move(sig)));
    return true;
}

const Signature *
EmbeddedSignature::signature() const noexcept
{
    return signature_.get();
}

Signature *
EmbeddedSignature::signature() noexcept
{
    return signature_.get();
}

void
EmbeddedSignature::set_signature(const Signature &sig)
{
    signature_ = std::unique_ptr<Signature>(new Signature(sig));
    data_.clear();
}

RawPtr
EmbeddedSignature::clone() const
{
    return RawPtr(new EmbeddedSignature(*this));
}

/* Issuer fingerprint signature subpacket */
void
IssuerFingerprint::write_data()
{
    data_.resize(fp_.size() + 1);
    data_[0] = version_;
    memcpy(data_.data() + 1, fp_.data(), fp_.size());
}

bool
IssuerFingerprint::check_size(size_t size) const noexcept
{
    return (size >= 21) && (size <= PGP_MAX_FINGERPRINT_SIZE + 1);
}

bool
IssuerFingerprint::parse_data(const uint8_t *data, size_t size)
{
    version_ = data[0];
    fp_ = pgp::Fingerprint(data + 1, size - 1);
    return true;
}

RawPtr
IssuerFingerprint::clone() const
{
    return RawPtr(new IssuerFingerprint(*this));
}

List::List(const List &src)
{
    items.reserve(src.items.size());
    for (auto &item : src.items) {
        items.push_back(item->clone());
    }
}

List &
List::operator=(const List &src)
{
    if (&src == this) {
        return *this;
    }
    items.clear();
    items.reserve(src.items.size());
    for (auto &item : src.items) {
        items.push_back(item->clone());
    }
    return *this;
}

} // namespace sigsub
} // namespace pkt
} // namespace pgp
