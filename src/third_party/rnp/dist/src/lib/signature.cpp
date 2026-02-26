/*
 * Copyright (c) 2025 [Ribose Inc](https://www.ribose.com).
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

#include "signature.hpp"

namespace rnp {

Signature::Signature(const pgp::pkt::Signature &sigpkt)
    : sig(sigpkt), sigid(sig.get_id()), raw(sigpkt)
{
}

bool
Signature::is_cert() const
{
    switch (sig.type()) {
    case PGP_CERT_CASUAL:
    case PGP_CERT_GENERIC:
    case PGP_CERT_PERSONA:
    case PGP_CERT_POSITIVE:
        return true;
    default:
        return false;
    }
}

bool
Signature::is_revocation() const
{
    switch (sig.type()) {
    case PGP_SIG_REV_KEY:
    case PGP_SIG_REV_SUBKEY:
    case PGP_SIG_REV_CERT:
        return true;
    default:
        return false;
    }
}

bool
Signature::expired(uint64_t at) const
{
    /* sig expiration: absence of subpkt or 0 means it never expires */
    uint64_t expiration = sig.expiration();
    if (!expiration) {
        return false;
    }
    return expiration + sig.creation() < at;
}

static const id_str_pair revocation_code_map[] = {
  {PGP_REVOCATION_NO_REASON, "No reason specified"},
  {PGP_REVOCATION_SUPERSEDED, "Key is superseded"},
  {PGP_REVOCATION_COMPROMISED, "Key material has been compromised"},
  {PGP_REVOCATION_RETIRED, "Key is retired and no longer used"},
  {PGP_REVOCATION_NO_LONGER_VALID, "User ID information is no longer valid"},
  {0x00, NULL},
};

Revocation::Revocation(Signature &sig) : uid(sig.uid), sigid(sig.sigid)
{
    if (!sig.sig.has_subpkt(PGP_SIG_SUBPKT_REVOCATION_REASON)) {
        RNP_LOG("Warning: no revocation reason in the revocation");
        code = PGP_REVOCATION_NO_REASON;
    } else {
        code = sig.sig.revocation_code();
        reason = sig.sig.revocation_reason();
    }
    if (reason.empty()) {
        reason = id_str_pair::lookup(revocation_code_map, code);
    }
}

} // namespace rnp
