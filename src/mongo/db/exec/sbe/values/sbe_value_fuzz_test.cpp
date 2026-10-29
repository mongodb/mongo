// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/document_tree_generator.h"
#include "mongo/db/exec/sbe/values/value.h"

#include <cstdint>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace mongo::sbe {
namespace {

value::TagValueOwned toTagValueOwned(const ScalarSpec& spec) {
    return std::visit(
        [](auto&& v) -> value::TagValueOwned {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, NullSpec>)
                return value::TagValueOwned::null();
            else if constexpr (std::is_same_v<T, NothingSpec>)
                return value::TagValueOwned::nothing();
            else if constexpr (std::is_same_v<T, bool>)
                return value::TagValueOwned::boolean(v);
            else if constexpr (std::is_same_v<T, int32_t>)
                return value::TagValueOwned::numberInt32(v);
            else if constexpr (std::is_same_v<T, int64_t>)
                return value::TagValueOwned::numberInt64(v);
            else if constexpr (std::is_same_v<T, std::string>)
                return value::TagValueOwned::fromRaw(value::makeNewString(v));
            else
                return value::TagValueOwned::numberDouble(v);
        },
        spec);
}

/*
 * Fuzz property: For any pair of scalars, the copies remain valid after the originals are released,
 * and compareValue returns an ordering unless one of the operands is Nothing.
 */
void FuzzScalarCopyReleaseCompare(ScalarSpec specA, ScalarSpec specB) {
    auto a = toTagValueOwned(specA);
    auto b = toTagValueOwned(specB);

    auto aCopy = a.copy();
    auto bCopy = b.copy();
    a = value::TagValueOwned::nothing();
    b = value::TagValueOwned::nothing();

    value::TagValueOwned cmp = value::TagValueOwned::fromRaw(
        value::compareValue(aCopy.tag(), aCopy.value(), bCopy.tag(), bCopy.value()));
    ASSERT_TRUE(cmp.tag() == value::TypeTags::NumberInt32 || cmp.tag() == value::TypeTags::Nothing);
    if (cmp.tag() == value::TypeTags::Nothing) {
        ASSERT_TRUE(aCopy.tag() == value::TypeTags::Nothing ||
                    bCopy.tag() == value::TypeTags::Nothing);
    }
}
FUZZ_TEST(SBEValueFuzz, FuzzScalarCopyReleaseCompare)
    .WithDomains(scalarSpecDomain(), scalarSpecDomain());

/*
 * Fuzz property: For any generated document, every field converts to an owned sbe value whose copy
 * compares equal to the original and survives the original's release.
 */
void FuzzDocumentValueRoundTrip(DocumentSpec spec) {
    BSONObj doc = renderDocument(spec, 0);

    for (auto&& elem : doc) {
        value::TagValueOwned v = bson::convertToOwned(elem);
        auto cp = v.copy();
        // Release the original to verify the copy stays valid.
        v = value::TagValueOwned::nothing();

        value::TagValueOwned fresh = bson::convertToOwned(elem);
        value::TagValueOwned cmp = value::TagValueOwned::fromRaw(
            value::compareValue(cp.tag(), cp.value(), fresh.tag(), fresh.value()));

        ASSERT_EQ(cmp.tag(), value::TypeTags::NumberInt32);
        ASSERT_EQ(cmp.value(), 0);
    }
}
FUZZ_TEST(SBEValueFuzz, FuzzDocumentValueRoundTrip).WithDomains(documentNodesDomain());

}  // namespace
}  // namespace mongo::sbe
