// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/ndv/ndv_hashing.h"

#include "mongo/base/data_range.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/murmur3.h"

#include <cstddef>

namespace mongo::ce {
namespace {

// Persisted sketches are built from these hashes, so everything below is an on-disk contract:
// changes require a schema version bump. The golden values in ndv_hashing_test.cpp enforce this.
constexpr auto kKeyStringVersion = key_string::Version::V1;
constexpr size_t kHashSeed = 0;

}  // namespace

uint64_t hashValueForNdv(sbe::value::TagValueView value) {
    return hashValuesForNdv(std::span<const sbe::value::TagValueView>{&value, 1});
}

uint64_t hashValuesForNdv(std::span<const sbe::value::TagValueView> values) {
    tassert(13176300, "NDV hashing requires at least one value", !values.empty());

    key_string::Builder builder(kKeyStringVersion, key_string::ALL_ASCENDING);
    for (auto value : values) {
        sbe::value::appendToKeyString(builder, value);
    }
    const auto bytes = builder.finishAndGetBuffer();
    static_assert(sizeof(size_t) == sizeof(uint64_t),
                  "murmur3<8> returns size_t, which must carry the full 64-bit hash");
    return static_cast<uint64_t>(murmur3<8>(ConstDataRange(bytes.data(), bytes.size()), kHashSeed));
}

}  // namespace mongo::ce
