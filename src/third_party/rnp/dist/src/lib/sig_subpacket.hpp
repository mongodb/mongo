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

#ifndef RNP_SIG_SUBPACKET_HPP_
#define RNP_SIG_SUBPACKET_HPP_

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include "repgp/repgp_def.h"
#include "fingerprint.hpp"
#include "types.h"

/**
 * @brief Signature subpacket classes.
 */
namespace pgp {

namespace pkt {

namespace sigsub {

enum class Type : uint8_t {
    Unknown = 0,
    Reserved_1 = 1,
    CreationTime = 2,   /* signature creation time */
    ExpirationTime = 3, /* signature expiration time */
    ExportableCert = 4, /* exportable certification */
    Trust = 5,          /* trust signature */
    RegExp = 6,         /* regular expression */
    Revocable = 7,      /* revocable */
    Reserved_8 = 8,
    KeyExpirationTime = 9,   /* key expiration time */
    Placeholder = 10,        /* placeholder for backward compatibility */
    PreferredSymmetric = 11, /* preferred symmetric algs */
    RevocationKey = 12,      /* revocation key */
    Reserved_13 = 13,
    Reserved_14 = 14,
    Reserved_15 = 15,
    IssuerKeyID = 16, /* issuer key ID */
    Reserved_17 = 17,
    Reserved_18 = 18,
    Reserved_19 = 19,
    NotationData = 20,       /* notation data */
    PreferredHash = 21,      /* preferred hash algs */
    PreferredCompress = 22,  /* preferred compression algorithms */
    KeyserverPrefs = 23,     /* key server preferences */
    PreferredKeyserver = 24, /* preferred key Server */
    PrimaryUserID = 25,      /* primary user ID */
    PolicyURI = 26,          /* policy URI */
    KeyFlags = 27,           /* key flags */
    SignersUserID = 28,      /* signer's user ID */
    RevocationReason = 29,   /* reason for revocation */
    Features = 30,           /* features */
    SignatureTarget = 31,    /* signature target */
    EmbeddedSignature = 32,  /* embedded signature */
    IssuerFingerprint = 33,  /* issuer fingerprint */
    PreferredAEAD = 34,      /* preferred AEAD algorithms */
#if defined(ENABLE_CRYPTO_REFRESH)
    /* IntendedRecipient = 35, */
    PreferredAEADv6 = 39,
#endif
    Private_100 = 100, /* private/experimental subpackets */
    Private_101 = 101,
    Private_102 = 102,
    Private_103 = 103,
    Private_104 = 104,
    Private_105 = 105,
    Private_106 = 106,
    Private_107 = 107,
    Private_108 = 108,
    Private_109 = 109,
    Private_110 = 110
};

/* Raw unparsed signature subpacket */
class Raw;
using RawPtr = std::unique_ptr<Raw>;

class Raw {
  private:
    bool    hashed_;
    uint8_t raw_type_;
    bool    critical_;

  protected:
    Type                 type_;
    std::vector<uint8_t> data_;

    virtual void write_data(){};
    virtual bool check_size(size_t size) const noexcept;
    virtual bool parse_data(const uint8_t *data, size_t size);

  public:
    Raw(uint8_t rawtype, bool hashed, bool critical);
    Raw(Type type, bool hashed, bool critical);
    virtual ~Raw();

    uint8_t
    raw_type() const noexcept
    {
        return raw_type_;
    }

    const std::vector<uint8_t> &
    data() const noexcept
    {
        return data_;
    }

    Type
    type() const noexcept
    {
        return type_;
    }

    bool
    critical() const noexcept
    {
        return critical_;
    }

    void
    set_critical(bool value) noexcept
    {
        critical_ = value;
    }

    bool
    hashed() const noexcept
    {
        return hashed_;
    }

    virtual const std::vector<uint8_t> &write();

    virtual bool parse(const uint8_t *data, size_t size);

    static RawPtr create(uint8_t type, bool hashed = true, bool critical = false);

    static RawPtr create(Type type, bool hashed = true, bool critical = false);

    static RawPtr create(const uint8_t *data, size_t size, bool hashed);

    virtual RawPtr clone() const;
};

/* Base class for timestamp-based subpackets */
class Time : public Raw {
  private:
    uint32_t time_;

  protected:
    void write_data() override;
    bool check_size(size_t size) const noexcept override;
    bool parse_data(const uint8_t *data, size_t size) override;

  public:
    Time(Type type, bool hashed, bool critical) : Raw(type, hashed, critical), time_(0)
    {
    }

    uint32_t
    time() const noexcept
    {
        return time_;
    }

    void
    set_time(uint32_t time) noexcept
    {
        time_ = time;
        data_.clear();
    }

    RawPtr clone() const override = 0;
};

/* Creation time signature subpacket */
class CreationTime : public Time {
  public:
    CreationTime(bool hashed = true, bool critical = false)
        : Time(Type::CreationTime, hashed, critical)
    {
    }

    RawPtr clone() const override;
};

/* Signature expiration time signature subpacket */
class ExpirationTime : public Time {
  public:
    ExpirationTime(bool hashed = true, bool critical = false)
        : Time(Type::ExpirationTime, hashed, critical)
    {
    }

    RawPtr clone() const override;
};

/* Key expiration time signature subpacket */
class KeyExpirationTime : public Time {
  public:
    KeyExpirationTime(bool hashed = true, bool critical = false)
        : Time(Type::KeyExpirationTime, hashed, critical)
    {
    }

    RawPtr clone() const override;
};

/* Base class for subpackets with single bool value */
class Bool : public Raw {
  protected:
    bool value_;

    void write_data() override;
    bool check_size(size_t size) const noexcept override;
    bool parse_data(const uint8_t *data, size_t size) override;

  public:
    Bool(Type type, bool hashed, bool critical) : Raw(type, hashed, critical), value_(false)
    {
    }

    RawPtr clone() const override = 0;
};

/* Exportable certification signature subpacket */
class ExportableCert : public Bool {
  public:
    ExportableCert(bool hashed = true, bool critical = false)
        : Bool(Type::ExportableCert, hashed, critical)
    {
        value_ = true;
    }

    bool
    exportable() const noexcept
    {
        return value_;
    }

    void
    set_exportable(bool exportable) noexcept
    {
        value_ = exportable;
        data_.clear();
    }

    RawPtr clone() const override;
};

/* Trust signature subpacket */
class Trust : public Raw {
  private:
    uint8_t level_;
    uint8_t amount_;

  protected:
    void write_data() override;
    bool check_size(size_t size) const noexcept override;
    bool parse_data(const uint8_t *data, size_t size) override;

  public:
    Trust(bool hashed = true, bool critical = false)
        : Raw(Type::Trust, hashed, critical), level_(0), amount_(0)
    {
    }

    uint8_t
    level() const noexcept
    {
        return level_;
    }

    uint8_t
    amount() const noexcept
    {
        return amount_;
    }

    void
    set_level(uint8_t level)
    {
        level_ = level;
        data_.clear();
    }

    void
    set_amount(uint8_t amount)
    {
        amount_ = amount;
        data_.clear();
    }

    RawPtr clone() const override;
};

/* Base class for single string value subpackets */
class String : public Raw {
  protected:
    std::string value_;

  protected:
    void write_data() override;
    bool parse_data(const uint8_t *data, size_t size) override;

  public:
    String(Type type, bool hashed, bool critical) : Raw(type, hashed, critical)
    {
    }

    RawPtr clone() const override = 0;
};

/* Regular expression signature subpacket */
class RegExp : public String {
  public:
    RegExp(bool hashed = true, bool critical = false) : String(Type::RegExp, hashed, critical)
    {
    }

    const std::string &
    regexp() const noexcept
    {
        return value_;
    }

    RawPtr clone() const override;
};

/* Revocable signature subpacket */
class Revocable : public Bool {
  public:
    Revocable(bool hashed = true, bool critical = false)
        : Bool(Type::Revocable, hashed, critical)
    {
        value_ = true;
    }

    bool
    revocable() const noexcept
    {
        return value_;
    }

    void
    set_revocable(bool revocable) noexcept
    {
        value_ = revocable;
        data_.clear();
    }

    RawPtr clone() const override;
};

/* Base class for preferred algorithms */
class Preferred : public Raw {
  private:
    std::vector<uint8_t> algs_;

  protected:
    void write_data() override;
    bool check_size(size_t size) const noexcept override;
    bool parse_data(const uint8_t *data, size_t size) override;

  public:
    Preferred(Type type, bool hashed, bool critical) : Raw(type, hashed, critical){};

    const std::vector<uint8_t> &
    algs() const noexcept
    {
        return algs_;
    }

    void
    set_algs(const std::vector<uint8_t> &algs)
    {
        algs_ = algs;
        data_.clear();
    }

    RawPtr clone() const override = 0;
};

/* Preferred symmetric algorithms */
class PreferredSymmetric : public Preferred {
  public:
    PreferredSymmetric(bool hashed = true, bool critical = false)
        : Preferred(Type::PreferredSymmetric, hashed, critical){};

    RawPtr clone() const override;
};

/* Preferred hash algorithms */
class PreferredHash : public Preferred {
  public:
    PreferredHash(bool hashed = true, bool critical = false)
        : Preferred(Type::PreferredHash, hashed, critical){};

    RawPtr clone() const override;
};

/* Preferred symmetric algorithms */
class PreferredCompress : public Preferred {
  public:
    PreferredCompress(bool hashed = true, bool critical = false)
        : Preferred(Type::PreferredCompress, hashed, critical){};

    RawPtr clone() const override;
};

/* Preferred AEAD algorithms for LibrePGP/pre-2021 crypto-refresh draft */
class PreferredAEAD : public Preferred {
  public:
    PreferredAEAD(bool hashed = true, bool critical = false)
        : Preferred(Type::PreferredAEAD, hashed, critical){};

    RawPtr clone() const override;
};

#if defined(ENABLE_CRYPTO_REFRESH)
/* Preferred AEAD algorithms as per RFC 9580 */
class PreferredAEADv6 : public Preferred {
  protected:
    bool check_size(size_t size) const noexcept override;

  public:
    PreferredAEADv6(bool hashed = true, bool critical = false)
        : Preferred(Type::PreferredAEADv6, hashed, critical){};

    RawPtr clone() const override;
};
#endif

/* Revocation key signature subpacket */
class RevocationKey : public Raw {
  private:
    uint8_t          rev_class_;
    pgp_pubkey_alg_t alg_;
    Fingerprint      fp_;

  protected:
    void write_data() override;
    bool check_size(size_t size) const noexcept override;
    bool parse_data(const uint8_t *data, size_t size) override;

  public:
    RevocationKey(bool hashed = true, bool critical = false)
        : Raw(Type::RevocationKey, hashed, critical), rev_class_(0),
          alg_(PGP_PKA_NOTHING), fp_{} {};

    uint8_t
    rev_class() const noexcept
    {
        return rev_class_;
    }

    void
    set_rev_class(uint8_t value) noexcept
    {
        rev_class_ = value;
        data_.clear();
    }

    pgp_pubkey_alg_t
    alg() const noexcept
    {
        return alg_;
    }

    void
    set_alg(pgp_pubkey_alg_t value) noexcept
    {
        alg_ = value;
        data_.clear();
    }

    const Fingerprint &
    fp() const noexcept
    {
        return fp_;
    }

    void
    set_fp(const Fingerprint &fp)
    {
        fp_ = fp;
        data_.clear();
    }

    RawPtr clone() const override;
};

/* Issuer Key ID signature subpacet */
class IssuerKeyID : public Raw {
  private:
    KeyID keyid_;

  protected:
    void write_data() override;
    bool check_size(size_t size) const noexcept override;
    bool parse_data(const uint8_t *data, size_t size) override;

  public:
    IssuerKeyID(bool hashed = true, bool critical = false)
        : Raw(Type::IssuerKeyID, hashed, critical)
    {
    }

    const KeyID &
    keyid() const noexcept
    {
        return keyid_;
    }

    void
    set_keyid(const KeyID &value) noexcept
    {
        keyid_ = value;
        data_.clear();
    }

    RawPtr clone() const override;
};

/* Notation data signature subpacket */
class NotationData : public Raw {
  private:
    std::array<uint8_t, 4> flags_;
    std::string            name_;
    std::vector<uint8_t>   value_;

  protected:
    void write_data() override;
    bool check_size(size_t size) const noexcept override;
    bool parse_data(const uint8_t *data, size_t size) override;

  public:
    NotationData(bool hashed = true, bool critical = false)
        : Raw(Type::NotationData, hashed, critical)
    {
    }

    bool
    human_readable() const noexcept
    {
        return flags_[0] & 0x80;
    }

    void set_human_readable(bool value) noexcept;

    const std::string &
    name() const noexcept
    {
        return name_;
    }

    void set_name(const std::string &name);

    const std::vector<uint8_t> &
    value() const noexcept
    {
        return value_;
    }

    void set_value(const std::vector<uint8_t> &value);

    RawPtr clone() const override;
};

/* Base class for bit flags subpackets */
class Flags : public Raw {
  protected:
    uint8_t flags_;

    void write_data() override;
    bool check_size(size_t size) const noexcept override;
    bool parse_data(const uint8_t *data, size_t size) override;

  public:
    Flags(Type type, bool hashed = true, bool critical = false)
        : Raw(type, hashed, critical), flags_(0)
    {
    }

    RawPtr clone() const override = 0;
};

/* Key server preferences signature subpacket */
class KeyserverPrefs : public Flags {
  public:
    KeyserverPrefs(bool hashed = true, bool critical = false)
        : Flags(Type::KeyserverPrefs, hashed, critical)
    {
    }

    bool
    no_modify() const noexcept
    {
        return flags_ & 0x80;
    }

    void
    set_no_modify(bool value) noexcept
    {
        flags_ = value ? 0x80 : 0;
        data_.clear();
    }

    uint8_t
    raw() const noexcept
    {
        return flags_;
    }

    void
    set_raw(uint8_t value) noexcept
    {
        flags_ = value;
        data_.clear();
    }

    RawPtr clone() const override;
};

/* Preferred key server signature subpacket */
class PreferredKeyserver : public String {
  public:
    PreferredKeyserver(bool hashed = true, bool critical = false)
        : String(Type::PreferredKeyserver, hashed, critical)
    {
    }

    const std::string &
    keyserver() const noexcept
    {
        return value_;
    }

    void
    set_keyserver(const std::string &value)
    {
        value_ = value;
        data_.clear();
    }

    RawPtr clone() const override;
};

/* Primary userid signature subpacket */
class PrimaryUserID : public Bool {
  public:
    PrimaryUserID(bool hashed = true, bool critical = false)
        : Bool(Type::PrimaryUserID, hashed, critical)
    {
    }

    bool
    primary() const noexcept
    {
        return value_;
    }

    void
    set_primary(bool primary) noexcept
    {
        value_ = primary;
        data_.clear();
    }

    RawPtr clone() const override;
};

/* Policy URI signature subpacket */
class PolicyURI : public String {
  public:
    PolicyURI(bool hashed = true, bool critical = false)
        : String(Type::PolicyURI, hashed, critical)
    {
    }

    const std::string &
    URI() const noexcept
    {
        return value_;
    }

    RawPtr clone() const override;
};

/* Key flags signature subpacket */
class KeyFlags : public Flags {
  public:
    KeyFlags(bool hashed = true, bool critical = false)
        : Flags(Type::KeyFlags, hashed, critical)
    {
    }

    uint8_t
    flags() const noexcept
    {
        return flags_;
    }

    void
    set_flags(uint8_t value) noexcept
    {
        flags_ = value;
        data_.clear();
    }

    RawPtr clone() const override;
};

/* Signer's user ID signature subpacket */
class SignersUserID : public String {
  public:
    SignersUserID(bool hashed = true, bool critical = false)
        : String(Type::SignersUserID, hashed, critical)
    {
    }

    const std::string &
    signer() const noexcept
    {
        return value_;
    }

    void
    set_signer(const std::string &value)
    {
        value_ = value;
        data_.clear();
    }

    RawPtr clone() const override;
};

/* Reason for revocation signature subpacket */
class RevocationReason : public Raw {
  private:
    pgp_revocation_type_t code_;
    std::string           reason_;

  protected:
    void write_data() override;
    bool check_size(size_t size) const noexcept override;
    bool parse_data(const uint8_t *data, size_t size) override;

  public:
    RevocationReason(bool hashed = true, bool critical = false)
        : Raw(Type::RevocationReason, hashed, critical), code_(PGP_REVOCATION_NO_REASON)
    {
    }

    pgp_revocation_type_t
    code() const noexcept
    {
        return code_;
    }

    void
    set_code(pgp_revocation_type_t value) noexcept
    {
        code_ = value;
        data_.clear();
    }

    const std::string &
    reason() const noexcept
    {
        return reason_;
    }

    void
    set_reason(const std::string &value)
    {
        reason_ = value;
        data_.clear();
    }

    RawPtr clone() const override;
};

/* Features signature subpacket */
class Features : public Flags {
  public:
    Features(bool hashed = true, bool critical = false)
        : Flags(Type::Features, hashed, critical)
    {
    }

    uint8_t
    features() const noexcept
    {
        return flags_;
    }

    void
    set_features(uint8_t value) noexcept
    {
        flags_ = value;
        data_.clear();
    }

    RawPtr clone() const override;
};

/* Embedded signature signature subpacket */
class EmbeddedSignature : public Raw {
  private:
    std::unique_ptr<Signature> signature_;

  protected:
    void write_data() override;
    bool check_size(size_t size) const noexcept override;
    bool parse_data(const uint8_t *data, size_t size) override;

  public:
    EmbeddedSignature(const EmbeddedSignature &src);
    EmbeddedSignature(bool hashed = true, bool critical = false);

    const Signature *signature() const noexcept;

    Signature *signature() noexcept;

    void set_signature(const Signature &sig);

    RawPtr clone() const override;
};

/* Issuer fingerprint signature subpacket */
class IssuerFingerprint : public Raw {
  private:
    uint8_t     version_;
    Fingerprint fp_;

  protected:
    void write_data() override;
    bool check_size(size_t size) const noexcept override;
    bool parse_data(const uint8_t *data, size_t size) override;

  public:
    IssuerFingerprint(bool hashed = true, bool critical = false)
        : Raw(Type::IssuerFingerprint, hashed, critical), version_(0), fp_{}
    {
    }

    uint8_t
    version() const noexcept
    {
        return version_;
    }

    void
    set_version(uint8_t version) noexcept
    {
        version_ = version;
        data_.clear();
    }

    const Fingerprint &
    fp() const noexcept
    {
        return fp_;
    }

    void
    set_fp(const Fingerprint &fp) noexcept
    {
        fp_ = fp;
        data_.clear();
    }

    RawPtr clone() const override;
};

class List {
  public:
    std::vector<RawPtr> items;

    List()
    {
    }
    List(const List &src);
    List &operator=(const List &src);

    std::vector<RawPtr>::iterator
    begin()
    {
        return items.begin();
    }

    std::vector<RawPtr>::iterator
    end()
    {
        return items.end();
    }

    std::vector<RawPtr>::const_iterator
    begin() const
    {
        return items.begin();
    }

    std::vector<RawPtr>::const_iterator
    end() const
    {
        return items.end();
    }

    size_t
    size() const
    {
        return items.size();
    }

    RawPtr &
    operator[](size_t idx)
    {
        return items[idx];
    }

    const RawPtr &
    operator[](size_t idx) const
    {
        return items[idx];
    }
};

} // namespace sigsub
} // namespace pkt
} // namespace pgp

#endif