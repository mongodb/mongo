// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/crypto/hash_block.h"
#include "mongo/util/make_array_type.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {
using namespace std::literals::string_view_literals;

/**
 * A Traits type for adapting HashBlock to sha256 hashes.
 */
struct SHA256BlockTraits {
    using HashType = MakeArrayType<std::uint8_t, 32, SHA256BlockTraits>;

    static constexpr std::string_view name = "SHA256Block"sv;

    static void computeHash(std::initializer_list<ConstDataRange> input, HashType* output);

    static void computeHashWithCtx(HashContext* ctx,
                                   std::initializer_list<ConstDataRange> input,
                                   HashType* output);

    static void computeHmac(const uint8_t* key,
                            size_t keyLen,
                            std::initializer_list<ConstDataRange> input,
                            HashType* output);

    static void computeHmacWithCtx(HmacContext* ctx,
                                   const uint8_t* key,
                                   size_t keyLen,
                                   std::initializer_list<ConstDataRange> input,
                                   HashType* output);
};

using SHA256Block = HashBlock<SHA256BlockTraits>;

}  // namespace mongo
