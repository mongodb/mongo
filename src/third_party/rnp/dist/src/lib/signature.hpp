/*
 * Copyright (c) 2017-2025 [Ribose Inc](https://www.ribose.com).
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

#ifndef RNP_SIGNATURE_HPP_
#define RNP_SIGNATURE_HPP_

#include <vector>
#include "librepgp/stream-sig.h"
#include "rawpacket.hpp"

namespace rnp {

class SigValidity {
    std::vector<int> errors_; /* signature validation errors */
    bool             validated_;

  public:
    SigValidity() : validated_(false){};

    const std::vector<int> &
    errors() const noexcept
    {
        return errors_;
    }

    void
    add_error(int err)
    {
        errors_.push_back(err);
    }

    /* signature doesn't have any validation problems and is validated */
    bool
    valid() const noexcept
    {
        return validated_ && errors_.empty();
    }

    bool
    validated() const noexcept
    {
        return validated_;
    }

    void
    mark_validated(rnp_result_t err = RNP_SUCCESS) noexcept
    {
        if (err) {
            errors_.push_back(err);
        }
        validated_ = true;
    }

    void
    reset(bool mark_valid = false)
    {
        errors_.clear();
        validated_ = mark_valid;
    }

    bool
    unknown() const noexcept
    {
        return (errors_.size() == 1) && (errors_[0] == RNP_ERROR_SIG_PARSE_ERROR);
    }

    bool
    expired() const noexcept
    {
        bool res = false;
        for (auto &err : errors_) {
            /* only these two cases were used as expired value */
            if ((err != RNP_ERROR_SIG_FROM_FUTURE) && (err != RNP_ERROR_SIG_EXPIRED)) {
                return false;
            }
            res = true;
        }
        return res;
    }

    bool
    no_signer() const noexcept
    {
        return (errors_.size() == 1) && (errors_[0] == RNP_ERROR_SIG_NO_SIGNER_KEY);
    }
};

class SignatureInfo {
  public:
    bool                 signer_valid{};      /* assume that signing key is valid */
    bool                 ignore_expiry{};     /* ignore signer's key expiration time */
    bool                 ignore_sig_expiry{}; /* we ignore expiration for revocations */
    pgp::pkt::Signature *sig{}; /* signature, or NULL if there were parsing error */
    SigValidity          validity;
};

class Signature {
  public:
    uint32_t            uid{};    /* index in userid array in key for certification sig */
    pgp::pkt::Signature sig{};    /* signature packet */
    pgp::SigID          sigid{};  /* signature identifier */
    RawPacket           raw;      /* signature's rawpacket */
    SigValidity         validity; /* signature validity information */

    Signature() = delete;
    Signature(const pgp::pkt::Signature &sig);

    /** @brief Returns true if signature is certification */
    bool is_cert() const;
    bool is_revocation() const;
    /** @brief Returns true if signature is expired */
    bool expired(uint64_t at) const;
};

class Revocation {
  public:
    uint32_t              uid;    /* index in uid array */
    pgp_revocation_type_t code;   /* revocation code */
    std::string           reason; /* revocation reason */
    pgp::SigID            sigid;  /* id of the corresponding subsig */

    Revocation() : uid(0), code(PGP_REVOCATION_NO_REASON), sigid(){};
    Revocation(Signature &sig);
};

} // namespace rnp

#endif