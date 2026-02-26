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

#include "userid.hpp"
#include <algorithm>
#include <stdexcept>

namespace rnp {

UserID::UserID(const pgp_userid_pkt_t &uidpkt) : UserID()
{
    /* copy packet data */
    pkt = uidpkt;
    rawpkt = RawPacket(uidpkt);
    /* populate uid string */
    if (uidpkt.tag == PGP_PKT_USER_ID) {
        str.assign(uidpkt.uid.data(), uidpkt.uid.data() + uidpkt.uid.size());
    } else {
        str = "(photo)";
    }
}

size_t
UserID::sig_count() const
{
    return sigs_.size();
}

const pgp::SigID &
UserID::get_sig(size_t idx) const
{
    return sigs_.at(idx);
}

bool
UserID::has_sig(const pgp::SigID &id) const
{
    return std::find(sigs_.begin(), sigs_.end(), id) != sigs_.end();
}

void
UserID::add_sig(const pgp::SigID &sig, bool begin)
{
    size_t idx = begin ? 0 : sigs_.size();
    sigs_.insert(sigs_.begin() + idx, sig);
}

void
UserID::replace_sig(const pgp::SigID &id, const pgp::SigID &newsig)
{
    auto it = std::find(sigs_.begin(), sigs_.end(), id);
    if (it == sigs_.end()) {
        throw std::invalid_argument("id");
    }
    *it = newsig;
}

bool
UserID::del_sig(const pgp::SigID &id)
{
    auto it = std::find(sigs_.begin(), sigs_.end(), id);
    if (it == sigs_.end()) {
        return false;
    }
    sigs_.erase(it);
    return true;
}

void
UserID::clear_sigs()
{
    sigs_.clear();
}

} // namespace rnp
