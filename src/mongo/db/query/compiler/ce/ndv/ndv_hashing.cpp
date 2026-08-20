// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/ndv/ndv_hashing.h"

#include "mongo/base/data_range.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/murmur3.h"

#include <cstddef>
#include <string_view>

namespace mongo::ce {
namespace {

// Persisted sketches are built from these hashes, so everything below is an on-disk contract:
// changes require a schema version bump. The golden values in ndv_hashing_test.cpp enforce this.
constexpr auto kKeyStringVersion = key_string::Version::V1;
constexpr size_t kHashSeed = 0;

void appendValueForNdv(key_string::Builder& builder, const BSONElement& value) {
    // NDV rejects arrays upstream, and their KeyString encoding drops the element field names
    // that woCompare compares, so we can't promise woCompare equivalence for them.
    tassert(11207614, "NDV hashing does not support array values", value.type() != BSONType::array);
    if (value.eoo()) {
        // Missing has no KeyString encoding. Undefined is woCompare-equal to missing and
        // distinct from null, so encode it as that.
        builder.appendUndefined();
    } else if (value.type() == BSONType::dbRef) {
        // dbrefNS() is NUL-terminated and truncates namespaces with embedded NUL bytes, which
        // would conflate distinct DBRefs. Read the namespace with its explicit length instead.
        // The payload is: int32 length, ns bytes (with trailing NUL), 12-byte OID; so +4 skips
        // the length prefix and valuestrsize() - 1 drops the trailing NUL.
        builder.appendDBRef(BSONDBRef(
            std::string_view(value.value() + 4, static_cast<size_t>(value.valuestrsize() - 1)),
            value.dbrefOID()));
    } else {
        // Type information that doesn't affect woCompare ordering (Int32 vs Double etc.) goes
        // to TypeBits, which the finished buffer excludes; that's what makes woCompare-equal
        // values byte-identical.
        builder.appendBSONElement(value);
    }
}

}  // namespace

uint64_t hashValueForNdv(const BSONElement& value) {
    return hashValuesForNdv(std::span<const BSONElement>{&value, 1});
}

uint64_t hashValuesForNdv(std::span<const BSONElement> values) {
    tassert(13176300, "NDV hashing requires at least one value", !values.empty());
    key_string::Builder builder(kKeyStringVersion, key_string::ALL_ASCENDING);
    for (const auto& value : values) {
        appendValueForNdv(builder, value);
    }
    const auto bytes = builder.finishAndGetBuffer();
    static_assert(sizeof(size_t) == sizeof(uint64_t),
                  "murmur3<8> returns size_t, which must carry the full 64-bit hash");
    return static_cast<uint64_t>(murmur3<8>(ConstDataRange(bytes.data(), bytes.size()), kHashSeed));
}

}  // namespace mongo::ce
