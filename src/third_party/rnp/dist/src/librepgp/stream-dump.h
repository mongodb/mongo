/*
 * Copyright (c) 2018, [Ribose Inc](https://www.ribose.com).
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

#ifndef STREAM_DUMP_H_
#define STREAM_DUMP_H_

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include "json_object.h"
#include "json.h"
#include "rnp.h"
#include "stream-common.h"
#include "stream-packet.h"
#include "sig_subpacket.hpp"
#include "key_material.hpp"

namespace rnp {
class DumpContext {
  protected:
    bool          dump_mpi{};
    bool          dump_packets{};
    bool          dump_grips{};
    size_t        layers{};
    size_t        stream_pkts{};
    size_t        failures{};
    pgp_source_t &src;

    virtual rnp_result_t dump_raw_packets() = 0;
    void                 copy_params(const DumpContext &ctx);
    bool                 get_aead_hdr(pgp_aead_hdr_t &hdr);
    bool                 skip_cleartext();

  public:
    DumpContext(pgp_source_t &asrc) : src(asrc){};
    virtual ~DumpContext(){};

    void
    set_dump_grips(bool value) noexcept
    {
        dump_grips = value;
    }

    void
    set_dump_mpi(bool value) noexcept
    {
        dump_mpi = value;
    }

    void
    set_dump_packets(bool value) noexcept
    {
        dump_packets = value;
    }

    virtual rnp_result_t dump(bool raw_only = false) = 0;
};

class DumpContextDst : public DumpContext {
    pgp_dest_t dst{};

    void         dump_signature_subpacket(const pgp::pkt::sigsub::Raw &subpkt);
    void         dump_signature_subpackets(const pgp::pkt::Signature &sig, bool hashed);
    void         dump_signature_pkt(const pgp::pkt::Signature &sig);
    rnp_result_t dump_signature();
    void         dump_key_material(const pgp::KeyMaterial *material);
    rnp_result_t dump_key();
    rnp_result_t dump_userid();
    rnp_result_t dump_pk_session_key();
    rnp_result_t dump_sk_session_key();
    rnp_result_t dump_aead_encrypted();
    rnp_result_t dump_encrypted(int tag);
    rnp_result_t dump_one_pass();
    rnp_result_t dump_compressed();
    rnp_result_t dump_literal();
    rnp_result_t dump_marker();
    rnp_result_t dump_raw_packets() override;

  public:
    DumpContextDst(pgp_source_t &asrc, pgp_dest_t &adst);
    ~DumpContextDst();

    rnp_result_t dump(bool raw_only = false) override;
};

class DumpContextJson : public DumpContext {
    json_object **json;

    bool dump_signature_subpacket(const pgp::pkt::sigsub::Raw &subpkt, json_object *obj);
    json_object *dump_signature_subpackets(const pgp::pkt::Signature &sig);
    rnp_result_t dump_signature_pkt(const pgp::pkt::Signature &sig, json_object *pkt);
    rnp_result_t dump_signature(json_object *pkt);
    bool         dump_key_material(const pgp::KeyMaterial *material, json_object *jso);
    rnp_result_t dump_key(json_object *pkt);
    rnp_result_t dump_user_id(json_object *pkt);
    rnp_result_t dump_pk_session_key(json_object *pkt);
    rnp_result_t dump_sk_session_key(json_object *pkt);
    rnp_result_t dump_encrypted(json_object *pkt, pgp_pkt_type_t tag);
    rnp_result_t dump_one_pass(json_object *pkt);
    rnp_result_t dump_marker(json_object *pkt);
    rnp_result_t dump_compressed(json_object *pkt);
    rnp_result_t dump_literal(json_object *pkt);
    bool         dump_pkt_hdr(pgp_packet_hdr_t &hdr, json_object *pkt);
    rnp_result_t dump_raw_packets() override;

  public:
    DumpContextJson(pgp_source_t &asrc, json_object **ajson)
        : DumpContext(asrc), json(ajson){};

    rnp_result_t dump(bool raw_only = false) override;
};

} // namespace rnp

#endif
