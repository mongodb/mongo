/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "util/checksum.hpp"
#include "igzip_checksums.h"
#include "compression/inflate/isal_kernels_wrappers.hpp"

namespace qpl::ml::util {

auto adler32(uint8_t *const begin, uint32_t size, uint32_t seed) noexcept -> uint32_t {
    auto old_adler32 = seed;
    auto new_adler32 = seed & least_significant_16_bits;

    new_adler32 = (new_adler32 == adler32_mod - 1) ? 0 : new_adler32 + 1;
    old_adler32 = isal_adler32((old_adler32 & most_significant_16_bits) | new_adler32, begin, size);
    new_adler32 = (old_adler32 & least_significant_16_bits);
    new_adler32 = (new_adler32 == 0) ? adler32_mod - 1 : new_adler32 - 1;

    return (old_adler32 & most_significant_16_bits) | new_adler32;
}

} // namespace qpl::ml
