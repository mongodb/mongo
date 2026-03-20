/*
 * Copyright (c) 2017-2022 Ribose Inc.
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

#ifndef CRYPTO_HASH_H_
#define CRYPTO_HASH_H_

#include <repgp/repgp_def.h>
#include "types.h"
#include "config.h"
#include "mem.h"
#include <memory>
#include <vector>
#include <array>

/**
 * Output size (in bytes) of biggest supported hash algo
 */
#define PGP_MAX_HASH_SIZE (64)

namespace rnp {
class Hash {
  protected:
    pgp_hash_alg_t alg_;
    size_t         size_;
    Hash(pgp_hash_alg_t alg) : alg_(alg)
    {
        size_ = Hash::size(alg);
    };

  public:
    pgp_hash_alg_t alg() const;
    size_t         size() const;

    static std::unique_ptr<Hash>  create(pgp_hash_alg_t alg);
    virtual std::unique_ptr<Hash> clone() const = 0;

    virtual void add(const void *buf, size_t len) = 0;
    virtual void add(const std::vector<uint8_t> &val);
    virtual void add(uint32_t val);
    virtual void add(const pgp::mpi &mpi);
    virtual void finish(uint8_t *digest) = 0;

    std::vector<uint8_t> finish();
    rnp::secure_bytes    sec_finish();

    virtual ~Hash();

    /* Hash algorithm by string representation from cleartext-signed text */
    static pgp_hash_alg_t alg(const char *name);
    /* Hash algorithm representation for cleartext-signed text */
    static const char *name(pgp_hash_alg_t alg);
    /* Size of the hash algorithm output or 0 if algorithm is unknown */
    static size_t size(pgp_hash_alg_t alg);
};

class CRC24 {
  protected:
    CRC24(){};

  public:
    static std::unique_ptr<CRC24> create();

    virtual void                   add(const void *buf, size_t len) = 0;
    virtual std::array<uint8_t, 3> finish() = 0;

    virtual ~CRC24(){};
};

class HashList {
  public:
    std::vector<std::unique_ptr<Hash>> hashes;

    void        add_alg(pgp_hash_alg_t alg);
    const Hash *get(pgp_hash_alg_t alg) const;
    void        add(const void *buf, size_t len);
};

} // namespace rnp

#endif
