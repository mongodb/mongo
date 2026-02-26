/*
 * Copyright (c) 2022 Ribose Inc.
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

#ifndef CRYPTO_HASH_BOTAN_HPP_
#define CRYPTO_HASH_BOTAN_HPP_

#include "hash.hpp"
#include <botan/hash.h>

namespace rnp {
class Hash_Botan : public Hash {
  private:
    std::unique_ptr<Botan::HashFunction> fn_;

    Hash_Botan(pgp_hash_alg_t alg);
    Hash_Botan(const Hash_Botan &src);

  public:
    virtual ~Hash_Botan();

    static std::unique_ptr<Hash_Botan> create(pgp_hash_alg_t alg);
    std::unique_ptr<Hash>              clone() const override;

    void add(const void *buf, size_t len) override;
    void finish(uint8_t *digest) override;

    static const char *name_backend(pgp_hash_alg_t alg);
};

class CRC24_Botan : public CRC24 {
    std::unique_ptr<Botan::HashFunction> fn_;
    CRC24_Botan();

  public:
    virtual ~CRC24_Botan();

    static std::unique_ptr<CRC24_Botan> create();

    void                   add(const void *buf, size_t len) override;
    std::array<uint8_t, 3> finish() override;
};

} // namespace rnp

#endif