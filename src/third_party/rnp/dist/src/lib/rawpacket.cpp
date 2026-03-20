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

#include "rawpacket.hpp"
#include "librepgp/stream-sig.h"
#include "librepgp/stream-key.h"

namespace rnp {

RawPacket::RawPacket(const uint8_t *data, size_t len, pgp_pkt_type_t atag) : tag_(atag)
{
    if (data && len) {
        data_.assign(data, data + len);
    }
}

RawPacket::RawPacket(const pgp::pkt::Signature &sig)
{
    data_ = sig.write();
    tag_ = PGP_PKT_SIGNATURE;
}

RawPacket::RawPacket(pgp_key_pkt_t &key)
{
    rnp::MemoryDest dst;
    key.write(dst.dst());
    data_ = dst.to_vector();
    tag_ = key.tag;
}

RawPacket::RawPacket(const pgp_userid_pkt_t &uid)
{
    rnp::MemoryDest dst;
    uid.write(dst.dst());
    data_ = dst.to_vector();
    tag_ = uid.tag;
}

void
RawPacket::write(pgp_dest_t &dst) const
{
    dst_write(&dst, data_.data(), data_.size());
}

} // namespace rnp
