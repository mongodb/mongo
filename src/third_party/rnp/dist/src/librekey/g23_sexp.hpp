/*
 * Copyright (c) 2021, [Ribose Inc](https://www.ribose.com).
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
 * THIS SOFTWARE IS PROVIDED BY THE RIBOSE, INC. AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef RNP_G23_SEXP_HPP
#define RNP_G23_SEXP_HPP

#include "sexpp/sexp.h"
#include "sexpp/ext-key-format.h"

#define SXP_MAX_DEPTH 30

class gnupg_sexp_t;
typedef std::shared_ptr<gnupg_sexp_t> p_gnupg_sexp;

class gnupg_sexp_t : public sexp::sexp_list_t {
    /* write gnupg_sexp_t contents, adding padding, for the further encryption */
    rnp::secure_bytes write_padded(size_t padblock) const;

  public:
    void
    add(const std::string &str)
    {
        push_back(std::shared_ptr<sexp::sexp_string_t>(new sexp::sexp_string_t(str)));
    };
    void
    add(const uint8_t *data, size_t size)
    {
        push_back(std::shared_ptr<sexp::sexp_string_t>(new sexp::sexp_string_t(data, size)));
    };
    void         add(unsigned u);
    p_gnupg_sexp add_sub();
    void         add_mpi(const std::string &name, const pgp::mpi &val);
    void         add_curve(const std::string &name, pgp_curve_t curve);
    void         add_pubkey(const pgp_key_pkt_t &key);
    void         add_seckey(const pgp_key_pkt_t &key);
    void         add_protected_seckey(pgp_key_pkt_t &       seckey,
                                      const std::string &   password,
                                      rnp::SecurityContext &ctx);
    bool         parse(const char *r_bytes, size_t r_length, size_t depth = 1);
    bool         write(pgp_dest_t &dst) const noexcept;
};

class gnupg_extended_private_key_t : public ext_key_format::extended_private_key_t {
  public:
    bool parse(const char *r_bytes, size_t r_length, size_t depth = 1);
};

#endif
