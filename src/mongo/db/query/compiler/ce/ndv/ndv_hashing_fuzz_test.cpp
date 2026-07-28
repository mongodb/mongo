// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bson_mutator/bson_mutator.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/compiler/ce/ndv/ndv_hashing.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/shared_buffer.h"

#include <string>
#include <string_view>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace mongo::ce {
namespace {

/**
 * The central property, checked pairwise: hashes are equal iff woCompare() says the values are
 * equal. A failure means the encoding conflates distinct values. (A real murmur collision is
 * 2^-64 per pair and the avalanche gives the fuzzer nothing to search with; if one ever shows
 * up, triage it as that.)
 */
void assertHashEqualityMatchesWoCompare(const BSONObj& obj) {
    // Cap the element count so the pairwise check stays cheap on large inputs.
    constexpr size_t kMaxElements = 64;
    std::vector<BSONElement> elements;
    std::vector<uint64_t> hashes;
    for (const BSONElement& element : obj) {
        if (elements.size() == kMaxElements) {
            break;
        }
        // Arrays are outside the hash domain; see the header.
        if (element.type() == BSONType::array) {
            continue;
        }
        elements.push_back(element);
        hashes.push_back(hashValueForNdv(element));
    }

    for (size_t i = 0; i < elements.size(); ++i) {
        for (size_t j = 0; j < elements.size(); ++j) {
            const bool valuesEqual = elements[i].woCompare(elements[j],
                                                           false /* considerFieldName */,
                                                           nullptr /* comparator */) == 0;
            ASSERT_EQ(hashes[i] == hashes[j], valuesEqual)
                << "lhs=" << elements[i] << " rhs=" << elements[j];
        }
    }
}

/**
 * Structured domain: every generated input is valid BSON, so every iteration hits the property.
 */
void HashPropertyOnStructuredBson(ConstSharedBuffer input) {
    assertHashEqualityMatchesWoCompare(BSONObj(input));
}

#define ENABLE_FIELD_TYPE(Camel, cpptype, bsontype, defaultdomain) .With##Camel(#Camel)

FUZZ_TEST(NdvHashingFuzz, HashPropertyOnStructuredBson)
    .WithDomains(fuzztest::Arbitrary<mongo::ConstSharedBuffer>()
                     BSON_MUTATOR_EXPAND_FIELD_TYPES(ENABLE_FIELD_TYPE));

/**
 * Raw bytes: mutation on the buffer itself covers encodings near the validity boundary that the
 * structured domain never produces.
 */
void HashPropertyOnRawBytes(const std::string& data) {
    if (!validateBSON(data.data(), data.size()).isOK()) {
        return;
    }
    assertHashEqualityMatchesWoCompare(BSONObj(data.data()));
}

std::vector<std::string> seeds() {
    auto toBytes = [](const BSONObj& obj) {
        return std::string(obj.objdata(), obj.objsize());
    };
    // Exotic types the raw domain would practically never assemble from scratch; mutation
    // explores their neighborhoods from here. The DBRef namespace with an embedded NUL is where
    // a real truncation bug lived.
    BSONObjBuilder exotic;
    exotic.appendDBRef("a", std::string_view("db\0x", 4), OID("507f1f77bcf86cd799439011"));
    exotic.appendSymbol("b", "sym");
    exotic.appendCodeWScope("c", "function() {}", BSON("x" << 1));
    exotic.append("d", Decimal128("0.1"));
    exotic.appendUndefined("e");

    return {
        toBytes(BSON("a" << 1 << "b" << 1.0 << "c" << int64_t{1} << "d" << 2)),
        toBytes(BSON("a" << "abc"
                         << "b"
                         << "abd"
                         << "c" << BSONNULL)),
        toBytes(BSON("a" << BSON("x" << 1) << "b" << BSON_ARRAY(1 << 2) << "c" << MAXKEY)),
        toBytes(exotic.obj()),
    };
}

FUZZ_TEST(NdvHashingFuzz, HashPropertyOnRawBytes)
    .WithDomains(fuzztest::Arbitrary<std::string>().WithSeeds(seeds()));

}  // namespace
}  // namespace mongo::ce
