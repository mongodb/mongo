/*
 * Copyright (c) 2021-2022 Ribose Inc.
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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <cassert>
#include "logging.h"
#include "hash_sha1cd.hpp"

namespace rnp {
Hash_SHA1CD::Hash_SHA1CD() : Hash(PGP_HASH_SHA1)
{
    assert(size_ == 20);
    SHA1DCInit(&ctx_);
}

Hash_SHA1CD::Hash_SHA1CD(const Hash_SHA1CD &src) : Hash(PGP_HASH_SHA1)
{
    ctx_ = src.ctx_;
}

Hash_SHA1CD::~Hash_SHA1CD()
{
}

std::unique_ptr<Hash_SHA1CD>
Hash_SHA1CD::create()
{
    return std::unique_ptr<Hash_SHA1CD>(new Hash_SHA1CD());
}

std::unique_ptr<Hash>
Hash_SHA1CD::clone() const
{
    return std::unique_ptr<Hash>(new Hash_SHA1CD(*this));
}

/* This produces runtime error: load of misaligned address 0x60d0000030a9 for type 'const
 * uint32_t' (aka 'const unsigned int'), which requires 4 byte alignment */
#if defined(__clang__)
__attribute__((no_sanitize("undefined")))
#endif
void
Hash_SHA1CD::add(const void *buf, size_t len)
{
    SHA1DCUpdate(&ctx_, (const char *) buf, len);
}

#if defined(__clang__)
__attribute__((no_sanitize("undefined")))
#endif
void
Hash_SHA1CD::finish(uint8_t *digest)
{
    unsigned char fixed_digest[20];
    int           res = SHA1DCFinal(fixed_digest, &ctx_);
    if (res && digest) {
        /* Show warning only if digest is non-null */
        RNP_LOG("Warning! SHA1 collision detected and mitigated.");
    }
    if (res) {
        return;
    }
    if (digest) {
        memcpy(digest, fixed_digest, 20);
    }
}

} // namespace rnp
