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

#ifndef RNP_USERID_HPP_
#define RNP_USERID_HPP_

#include <vector>
#include "rawpacket.hpp"
#include "signature.hpp"
#include "librepgp/stream-packet.h"

namespace rnp {

/* userid, built on top of userid packet structure */
class UserID {
  private:
    pgp::SigIDs sigs_; /* all signatures related to this userid */
  public:
    pgp_userid_pkt_t pkt;    /* User ID or User Attribute packet as it was loaded */
    RawPacket        rawpkt; /* Raw packet contents */
    std::string      str;    /* Human-readable representation of the userid */
    bool             valid;  /* User ID is valid, i.e. has valid, non-expired self-signature */
    bool             revoked;
    Revocation       revocation;

    UserID() : valid(false), revoked(false){};
    UserID(const pgp_userid_pkt_t &pkt);

    size_t            sig_count() const;
    const pgp::SigID &get_sig(size_t idx) const;
    bool              has_sig(const pgp::SigID &id) const;
    void              add_sig(const pgp::SigID &sig, bool begin = false);
    void              replace_sig(const pgp::SigID &id, const pgp::SigID &newsig);
    bool              del_sig(const pgp::SigID &id);
    void              clear_sigs();

    /* No userid, i.e. direct-key signature */
    static const uint32_t None = (uint32_t) -1;
    /* look only for primary userids */
    static const uint32_t Primary = (uint32_t) -2;
    /* look for any uid, except UserID::None) */
    static const uint32_t Any = (uint32_t) -3;
};

} // namespace rnp

#endif