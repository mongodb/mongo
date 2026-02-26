/*-
 * Copyright (c) 2018-2025 Ribose Inc.
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

#include <string.h>
#include <stdlib.h>
#include "mpi.hpp"
#include "mem.h"
#include "utils.h"

namespace pgp {

size_t
mpi::bits() const noexcept
{
    size_t  bits = 0;
    size_t  idx = 0;
    uint8_t bt;

    for (idx = 0; (idx < size()) && !data_[idx]; idx++)
        ;

    if (idx < size()) {
        for (bits = (size() - idx - 1) << 3, bt = data_[idx]; bt; bits++, bt = bt >> 1)
            ;
    }

    return bits;
}

size_t
mpi::size() const noexcept
{
    return data_.size();
}

uint8_t *
mpi::data() noexcept
{
    return data_.data();
}

const uint8_t *
mpi::data() const noexcept
{
    return data_.data();
}

bool
mpi::operator==(const mpi &src) const
{
    size_t idx1 = 0;
    size_t idx2 = 0;

    for (idx1 = 0; (idx1 < size()) && !data_[idx1]; idx1++)
        ;

    for (idx2 = 0; (idx2 < src.size()) && !src[idx2]; idx2++)
        ;

    return ((size() - idx1) == (src.size() - idx2) &&
            !memcmp(data() + idx1, src.data() + idx2, size() - idx1));
}

bool
mpi::operator!=(const mpi &src) const
{
    return !(*this == src);
}

uint8_t &
mpi::operator[](size_t idx)
{
    return data_.at(idx);
}

const uint8_t &
mpi::operator[](size_t idx) const
{
    return data_.at(idx);
}

void
mpi::assign(const uint8_t *val, size_t size)
{
    data_.assign(val, val + size);
}

void
mpi::copy(uint8_t *dst) const noexcept
{
    memcpy(dst, data_.data(), data_.size());
}

void
mpi::resize(size_t size, uint8_t fill)
{
    data_.resize(size, fill);
}

void
mpi::forget() noexcept
{
    secure_clear(data_.data(), data_.size());
    data_.resize(0);
}

} // namespace pgp
