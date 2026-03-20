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
#ifndef RNP_RAWPACKET_HPP_
#define RNP_RAWPACKET_HPP_

#include <vector>
#include "types.h"
#include "librepgp/stream-common.h"

typedef struct pgp_key_pkt_t    pgp_key_pkt_t;
typedef struct pgp_userid_pkt_t pgp_userid_pkt_t;

namespace rnp {
class RawPacket {
    pgp_pkt_type_t       tag_;
    std::vector<uint8_t> data_;

  public:
    RawPacket() : tag_(PGP_PKT_RESERVED){};
    RawPacket(const uint8_t *data, size_t len, pgp_pkt_type_t atag);
    RawPacket(const pgp::pkt::Signature &sig);
    RawPacket(pgp_key_pkt_t &key);
    RawPacket(const pgp_userid_pkt_t &uid);

    const std::vector<uint8_t> &
    data() const noexcept
    {
        return data_;
    }

    pgp_pkt_type_t
    tag() const noexcept
    {
        return tag_;
    }

    void write(pgp_dest_t &dst) const;
};
} // namespace rnp

#endif
