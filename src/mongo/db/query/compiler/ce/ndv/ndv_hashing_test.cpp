// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/ndv/ndv_hashing.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"

#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace mongo::ce {
namespace {

/**
 * Single-value documents covering every relevant BSON type, with groups that woCompare treats as
 * equal despite different representations. Field names vary on purpose; they must not matter.
 */
std::vector<BSONObj> corpus() {
    std::vector<BSONObj> docs;

    // Numerics: one woCompare-equal group across all four numeric types.
    docs.push_back(BSON("a" << 1));
    docs.push_back(BSON("b" << int64_t{1}));
    docs.push_back(BSON("c" << 1.0));
    docs.push_back(BSON("d" << Decimal128("1.000")));
    docs.push_back(BSON("e" << 2));

    // 2^53 edge: the long and double 2^53 are equal, but 2^53 + 1 has no exact double form.
    docs.push_back(BSON("f" << int64_t{9'007'199'254'740'992}));
    docs.push_back(BSON("g" << 9'007'199'254'740'992.0));
    docs.push_back(BSON("h" << int64_t{9'007'199'254'740'993}));

    // NaN payloads and types all compare equal; +/-0.0 across types too.
    docs.push_back(BSON("i" << std::numeric_limits<double>::quiet_NaN()));
    docs.push_back(BSON("j" << -std::numeric_limits<double>::quiet_NaN()));
    docs.push_back(BSON("k" << Decimal128::kPositiveNaN));
    docs.push_back(BSON("l" << 0.0));
    docs.push_back(BSON("m" << -0.0));
    docs.push_back(BSON("n" << 0));

    // Strings and symbols: woCompare treats a Symbol as equal to the same String.
    docs.push_back(BSON("o" << "abc"));
    {
        BSONObjBuilder bob;
        bob.appendSymbol("p", "abc");
        docs.push_back(bob.obj());
    }
    docs.push_back(BSON("q" << "abd"));
    docs.push_back(BSON("r" << ""));

    // Booleans are their own canonical type: true must not equal 1.
    docs.push_back(BSON("s" << true));
    docs.push_back(BSON("t" << false));

    // Null and Undefined are distinct values.
    {
        BSONObjBuilder bob;
        bob.appendNull("u");
        docs.push_back(bob.obj());
    }
    {
        BSONObjBuilder bob;
        bob.appendUndefined("v");
        docs.push_back(bob.obj());
    }

    docs.push_back(BSON("w" << MINKEY));
    docs.push_back(BSON("x" << MAXKEY));

    // Date and Timestamp with the same numeric content are different types.
    docs.push_back(BSON("y" << Date_t::fromMillisSinceEpoch(12345)));
    docs.push_back(BSON("z" << Timestamp(12345, 1)));

    docs.push_back(BSON("aa" << OID("507f1f77bcf86cd799439011")));
    {
        BSONObjBuilder bob;
        constexpr uint8_t bytes[] = {1, 2, 3};
        bob.appendBinData("ab", sizeof(bytes), BinDataGeneral, bytes);
        docs.push_back(bob.obj());
    }
    {
        BSONObjBuilder bob;
        bob.appendRegex("ac", "^a", "i");
        docs.push_back(bob.obj());
    }
    {
        BSONObjBuilder bob;
        bob.appendCode("ad", "function() {}");
        docs.push_back(bob.obj());
    }
    {
        BSONObjBuilder bob;
        bob.appendCodeWScope("am", "function() {}", BSON("x" << 1));
        docs.push_back(bob.obj());
    }
    {
        BSONObjBuilder bob;
        bob.appendDBRef("an", "db.coll", OID("507f1f77bcf86cd799439011"));
        docs.push_back(bob.obj());
    }
    {
        // DBRef namespaces differing only after an embedded NUL; equality must see the whole
        // namespace.
        BSONObjBuilder bob;
        bob.appendDBRef("ba", std::string_view("db\0a", 4), OID("507f1f77bcf86cd799439011"));
        docs.push_back(bob.obj());
    }
    {
        BSONObjBuilder bob;
        bob.appendDBRef("bb", std::string_view("db\0b", 4), OID("507f1f77bcf86cd799439011"));
        docs.push_back(bob.obj());
    }

    // Objects as values: field order and nesting matter. (Arrays are outside the hash domain.)
    docs.push_back(BSON("ae" << BSONObj()));
    docs.push_back(BSON("af" << BSON("a" << 1)));
    docs.push_back(BSON("ag" << BSON("a" << 1 << "b" << 2)));
    docs.push_back(BSON("ah" << BSON("b" << 2 << "a" << 1)));
    docs.push_back(BSON("ai" << BSON("a" << BSON("b" << 1))));

    // Negative numbers: a woCompare-equal group across the numeric types, distinct from the
    // positive counterparts.
    docs.push_back(BSON("ao" << -1));
    docs.push_back(BSON("ap" << int64_t{-1}));
    docs.push_back(BSON("aq" << -1.0));
    docs.push_back(BSON("ar" << Decimal128("-1.000")));

    // Decimal zeros, including negative zero, join the zero group.
    docs.push_back(BSON("as" << Decimal128("-0")));
    docs.push_back(BSON("at" << Decimal128("0.000")));

    // Doubles vs decimals near representability boundaries: the double literals are not exactly
    // 1/10 or 10^300, so they must not collide with the exact Decimal128 values; plus a
    // high-precision decimal at the top of the Decimal128 range.
    docs.push_back(BSON("au" << 0.1));
    docs.push_back(BSON("av" << Decimal128("0.1")));
    docs.push_back(BSON("aw" << 1e300));
    docs.push_back(BSON("ax" << Decimal128("1E+300")));
    docs.push_back(BSON("ay" << Decimal128("9.999999999999999999999999999999999E+6144")));

    // Embedded NUL bytes are legal in BSON strings; the encoding must not truncate at them.
    docs.push_back(BSON("az" << std::string("a\0b", 3)));
    docs.push_back(BSON("ba" << std::string("a\0c", 3)));
    docs.push_back(BSON("bb" << "a"));

    // Long strings differing only in the last byte.
    docs.push_back(BSON("bc" << std::string(2048, 'x')));
    docs.push_back(BSON("bd" << std::string(2047, 'x') + 'y'));

    // Deep nesting: two identical deep objects and one differing only in the innermost leaf.
    auto makeDeepObject = [](int leaf) {
        BSONObj nested = BSON("v" << leaf);
        for (int i = 0; i < 30; ++i) {
            nested = BSON("v" << nested);
        }
        return nested;
    };
    docs.push_back(BSON("be" << makeDeepObject(1)));
    docs.push_back(BSON("bf" << makeDeepObject(1)));
    docs.push_back(BSON("bg" << makeDeepObject(2)));
    // An empty document's firstElement() is EOO, i.e. missing. woCompare treats it as equal to
    // Undefined.
    docs.push_back(BSONObj());

    return docs;
}

TEST(NdvHashingTest, HashEqualityMatchesWoCompareOverCorpus) {
    // The central correctness property: for every pair of corpus values, the hashes are equal iff
    // the values compare equal under woCompare with no collator and field names ignored. That is
    // the definition of distinctness that sample-based NDV estimation uses.
    const auto docs = corpus();
    for (size_t i = 0; i < docs.size(); ++i) {
        for (size_t j = 0; j < docs.size(); ++j) {
            const BSONElement lhs = docs[i].firstElement();
            const BSONElement rhs = docs[j].firstElement();
            const bool valuesEqual =
                lhs.woCompare(rhs, false /* considerFieldName */, nullptr /* comparator */) == 0;
            const bool hashesEqual = hashValueForNdv(lhs) == hashValueForNdv(rhs);
            ASSERT_EQ(hashesEqual, valuesEqual)
                << "lhs=" << docs[i] << " rhs=" << docs[j] << " valuesEqual=" << valuesEqual;
        }
    }
}

TEST(NdvHashingTest, FieldNamesDoNotParticipate) {
    const BSONObj shortName = BSON("a" << 42);
    const BSONObj longName = BSON("a_completely_different_field_name" << 42);
    ASSERT_EQ(hashValueForNdv(shortName.firstElement()), hashValueForNdv(longName.firstElement()));
}

TEST(NdvHashingTest, MissingHashMatchesWoCompareSemantics) {
    const uint64_t missingHash = hashValueForNdv(BSONElement{});

    // Missing is woCompare-equal to Undefined, so the hashes must match.
    BSONObjBuilder undefinedBob;
    undefinedBob.appendUndefined("u");
    ASSERT_EQ(missingHash, hashValueForNdv(undefinedBob.obj().firstElement()));

    // But distinct from null.
    BSONObjBuilder nullBob;
    nullBob.appendNull("n");
    ASSERT_NE(missingHash, hashValueForNdv(nullBob.obj().firstElement()));
}

TEST(NdvHashingTest, HashesAreStable) {
    // These freeze the on-disk hash contract. If this test breaks, previously persisted sketches
    // are silently invalidated: don't update the values without a schema version bump.
    ASSERT_EQ(hashValueForNdv(BSON("a" << 1).firstElement()), 5'414'143'151'907'426'158ULL);
    ASSERT_EQ(hashValueForNdv(BSON("a" << "abc").firstElement()), 11'132'583'522'755'588'235ULL);
    BSONObjBuilder bob;
    bob.appendNull("a");
    ASSERT_EQ(hashValueForNdv(bob.obj().firstElement()), 6'893'204'474'022'492'526ULL);
    ASSERT_EQ(hashValueForNdv(BSONElement{}),
              5'663'883'736'451'043'293ULL);  // Missing, identical to Undefined.
}

}  // namespace
}  // namespace mongo::ce
