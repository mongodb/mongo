/**
 *    Copyright (C) 2015 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/plan_cache_indexability.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

std::unique_ptr<MatchExpression> parseMatchExpression(const BSONObj& obj,
                                                      const CollatorInterface* collator = nullptr) {
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(obj, ExtensionsCallbackDisallowExtensions(), collator);
    if (!status.isOK()) {
        FAIL(str::stream() << "failed to parse query: " << obj.toString() << ". Reason: "
                           << status.getStatus().toString());
    }
    return std::move(status.getValue());
}

// Test sparse index discriminators for a simple sparse index.
TEST(PlanCacheIndexabilityTest, SparseIndexSimple) {
    PlanCacheIndexabilityState state;
    state.updateDiscriminators({IndexEntry(BSON("a" << 1),
                                           false,    // multikey
                                           true,     // sparse
                                           false,    // unique
                                           "a_1",    // name
                                           nullptr,  // filterExpr
                                           BSONObj())});

    auto discriminators = state.getDiscriminators("a");
    ASSERT_EQ(1U, discriminators.size());
    ASSERT(discriminators.find("a_1") != discriminators.end());

    auto disc = discriminators["a_1"];
    ASSERT_EQ(true, disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("a" << 1)).get()));
    ASSERT_EQ(false,
              disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("a" << BSONNULL)).get()));
    ASSERT_EQ(true,
              disc.isMatchCompatibleWithIndex(
                  parseMatchExpression(BSON("a" << BSON("$in" << BSON_ARRAY(1)))).get()));
    ASSERT_EQ(false,
              disc.isMatchCompatibleWithIndex(
                  parseMatchExpression(BSON("a" << BSON("$in" << BSON_ARRAY(BSONNULL)))).get()));
}

// Test sparse index discriminators for a compound sparse index.
TEST(PlanCacheIndexabilityTest, SparseIndexCompound) {
    PlanCacheIndexabilityState state;
    state.updateDiscriminators({IndexEntry(BSON("a" << 1 << "b" << 1),
                                           false,      // multikey
                                           true,       // sparse
                                           false,      // unique
                                           "a_1_b_1",  // name
                                           nullptr,    // filterExpr
                                           BSONObj())});

    {
        auto discriminators = state.getDiscriminators("a");
        ASSERT_EQ(1U, discriminators.size());
        ASSERT(discriminators.find("a_1_b_1") != discriminators.end());

        auto disc = discriminators["a_1_b_1"];
        ASSERT_EQ(true,
                  disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("a" << 1)).get()));
        ASSERT_EQ(
            false,
            disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("a" << BSONNULL)).get()));
    }

    {
        auto discriminators = state.getDiscriminators("b");
        ASSERT_EQ(1U, discriminators.size());
        ASSERT(discriminators.find("a_1_b_1") != discriminators.end());

        auto disc = discriminators["a_1_b_1"];
        ASSERT_EQ(true,
                  disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("b" << 1)).get()));
        ASSERT_EQ(
            false,
            disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("b" << BSONNULL)).get()));
    }
}

// Test partial index discriminators for an index with a simple filter.
TEST(PlanCacheIndexabilityTest, PartialIndexSimple) {
    BSONObj filterObj = BSON("f" << BSON("$gt" << 0));
    std::unique_ptr<MatchExpression> filterExpr(parseMatchExpression(filterObj));
    PlanCacheIndexabilityState state;
    state.updateDiscriminators({IndexEntry(BSON("a" << 1),
                                           false,  // multikey
                                           false,  // sparse
                                           false,  // unique
                                           "a_1",  // name
                                           filterExpr.get(),
                                           BSONObj())});

    {
        auto discriminators = state.getDiscriminators("f");
        ASSERT_EQ(1U, discriminators.size());
        ASSERT(discriminators.find("a_1") != discriminators.end());

        auto disc = discriminators["a_1"];
        ASSERT_EQ(false,
                  disc.isMatchCompatibleWithIndex(
                      parseMatchExpression(BSON("f" << BSON("$gt" << -5))).get()));
        ASSERT_EQ(true,
                  disc.isMatchCompatibleWithIndex(
                      parseMatchExpression(BSON("f" << BSON("$gt" << 5))).get()));
    }

    {
        auto discriminators = state.getDiscriminators("a");
        ASSERT_EQ(1U, discriminators.size());
        ASSERT(discriminators.find("a_1") != discriminators.end());

        auto disc = discriminators["a_1"];
        ASSERT_EQ(true,
                  disc.isMatchCompatibleWithIndex(
                      parseMatchExpression(BSON("a" << BSON("$gt" << -5))).get()));
        ASSERT_EQ(true,
                  disc.isMatchCompatibleWithIndex(
                      parseMatchExpression(BSON("a" << BSON("$gt" << -5))).get()));
    }
}

// Test partial index discriminators for an index where the filter expression is an AND.
TEST(PlanCacheIndexabilityTest, PartialIndexAnd) {
    BSONObj filterObj = BSON("f" << 1 << "g" << 1);
    std::unique_ptr<MatchExpression> filterExpr(parseMatchExpression(filterObj));
    PlanCacheIndexabilityState state;
    state.updateDiscriminators({IndexEntry(BSON("a" << 1),
                                           false,  // multikey
                                           false,  // sparse
                                           false,  // unique
                                           "a_1",  // name
                                           filterExpr.get(),
                                           BSONObj())});

    {
        auto discriminators = state.getDiscriminators("f");
        ASSERT_EQ(1U, discriminators.size());
        ASSERT(discriminators.find("a_1") != discriminators.end());

        auto disc = discriminators["a_1"];
        ASSERT_EQ(false,
                  disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("f" << 0)).get()));
        ASSERT_EQ(true,
                  disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("f" << 1)).get()));
    }

    {
        auto discriminators = state.getDiscriminators("g");
        ASSERT_EQ(1U, discriminators.size());
        ASSERT(discriminators.find("a_1") != discriminators.end());

        auto disc = discriminators["a_1"];
        ASSERT_EQ(false,
                  disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("g" << 0)).get()));
        ASSERT_EQ(true,
                  disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("g" << 1)).get()));
    }

    {
        auto discriminators = state.getDiscriminators("a");
        ASSERT_EQ(1U, discriminators.size());
        ASSERT(discriminators.find("a_1") != discriminators.end());

        auto disc = discriminators["a_1"];
        ASSERT_EQ(true,
                  disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("a" << 0)).get()));
        ASSERT_EQ(true,
                  disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("a" << 1)).get()));
    }
}

// Test partial index discriminators where there are multiple partial indexes.
TEST(PlanCacheIndexabilityTest, MultiplePartialIndexes) {
    BSONObj filterObj1 = BSON("f" << 1);
    std::unique_ptr<MatchExpression> filterExpr1(parseMatchExpression(filterObj1));

    BSONObj filterObj2 = BSON("f" << 2);
    std::unique_ptr<MatchExpression> filterExpr2(parseMatchExpression(filterObj2));

    PlanCacheIndexabilityState state;
    state.updateDiscriminators({IndexEntry(BSON("a" << 1),
                                           false,  // multikey
                                           false,  // sparse
                                           false,  // unique
                                           "a_1",  // name
                                           filterExpr1.get(),
                                           BSONObj()),
                                IndexEntry(BSON("b" << 1),
                                           false,  // multikey
                                           false,  // sparse
                                           false,  // unique
                                           "b_1",  // name
                                           filterExpr2.get(),
                                           BSONObj())});

    {
        auto discriminators = state.getDiscriminators("f");
        ASSERT_EQ(2U, discriminators.size());
        ASSERT(discriminators.find("a_1") != discriminators.end());
        ASSERT(discriminators.find("b_1") != discriminators.end());

        auto discA = discriminators["a_1"];
        auto discB = discriminators["b_1"];

        ASSERT_EQ(false,
                  discA.isMatchCompatibleWithIndex(parseMatchExpression(BSON("f" << 0)).get()));
        ASSERT_EQ(false,
                  discB.isMatchCompatibleWithIndex(parseMatchExpression(BSON("f" << 0)).get()));

        ASSERT_EQ(true,
                  discA.isMatchCompatibleWithIndex(parseMatchExpression(BSON("f" << 1)).get()));
        ASSERT_EQ(false,
                  discB.isMatchCompatibleWithIndex(parseMatchExpression(BSON("f" << 1)).get()));

        ASSERT_EQ(false,
                  discA.isMatchCompatibleWithIndex(parseMatchExpression(BSON("f" << 2)).get()));
        ASSERT_EQ(true,
                  discB.isMatchCompatibleWithIndex(parseMatchExpression(BSON("f" << 2)).get()));
    }

    {
        auto discriminators = state.getDiscriminators("a");
        ASSERT_EQ(1U, discriminators.size());
        ASSERT(discriminators.find("a_1") != discriminators.end());

        auto disc = discriminators["a_1"];
        ASSERT_EQ(true,
                  disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("a" << 0)).get()));
        ASSERT_EQ(true,
                  disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("a" << 1)).get()));
        ASSERT_EQ(true,
                  disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("a" << 2)).get()));
    }

    {
        auto discriminators = state.getDiscriminators("b");
        ASSERT_EQ(1U, discriminators.size());
        ASSERT(discriminators.find("b_1") != discriminators.end());

        auto disc = discriminators["b_1"];
        ASSERT_EQ(true,
                  disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("b" << 0)).get()));
        ASSERT_EQ(true,
                  disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("b" << 1)).get()));
        ASSERT_EQ(true,
                  disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("b" << 2)).get()));
    }
}

// Test that a discriminator is generated for a regular index (this discriminator will only encode
// collation indexability).
TEST(PlanCacheIndexabilityTest, IndexNeitherSparseNorPartial) {
    PlanCacheIndexabilityState state;
    state.updateDiscriminators({IndexEntry(BSON("a" << 1),
                                           false,  // multikey
                                           false,  // sparse
                                           false,  // unique
                                           "a_1",  // name
                                           nullptr,
                                           BSONObj())});
    auto discriminators = state.getDiscriminators("a");
    ASSERT_EQ(1U, discriminators.size());
    ASSERT(discriminators.find("a_1") != discriminators.end());
}

// Test discriminator for a simple index with a collation.
TEST(PlanCacheIndexabilityTest, DiscriminatorForCollationIndicatesWhenCollationsAreCompatible) {
    PlanCacheIndexabilityState state;
    IndexEntry entry(BSON("a" << 1),
                     false,    // multikey
                     false,    // sparse
                     false,    // unique
                     "a_1",    // name
                     nullptr,  // filterExpr
                     BSONObj());
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    entry.collator = &collator;
    state.updateDiscriminators({entry});

    auto discriminators = state.getDiscriminators("a");
    ASSERT_EQ(1U, discriminators.size());
    ASSERT(discriminators.find("a_1") != discriminators.end());

    auto disc = discriminators["a_1"];

    // Index collator matches query collator.
    ASSERT_EQ(true,
              disc.isMatchCompatibleWithIndex(
                  parseMatchExpression(fromjson("{a: 'abc'}"), &collator).get()));
    ASSERT_EQ(true,
              disc.isMatchCompatibleWithIndex(
                  parseMatchExpression(fromjson("{a: {$in: ['abc', 'xyz']}}"), &collator).get()));

    // Expression is not a ComparisonMatchExpression or InMatchExpression.
    ASSERT_EQ(true,
              disc.isMatchCompatibleWithIndex(
                  parseMatchExpression(fromjson("{a: {$exists: true}}"), nullptr).get()));

    // Expression is a ComparisonMatchExpression with non-matching collator.
    ASSERT_EQ(
        true,
        disc.isMatchCompatibleWithIndex(parseMatchExpression(fromjson("{a: 5}"), nullptr).get()));
    ASSERT_EQ(false,
              disc.isMatchCompatibleWithIndex(
                  parseMatchExpression(fromjson("{a: 'abc'}"), nullptr).get()));
    ASSERT_EQ(false,
              disc.isMatchCompatibleWithIndex(
                  parseMatchExpression(fromjson("{a: {b: 'abc'}}"), nullptr).get()));
    ASSERT_EQ(false,
              disc.isMatchCompatibleWithIndex(
                  parseMatchExpression(fromjson("{a: ['abc', 'xyz']}"), nullptr).get()));

    // Expression is an InMatchExpression with non-matching collator.
    ASSERT_EQ(true,
              disc.isMatchCompatibleWithIndex(
                  parseMatchExpression(fromjson("{a: {$in: [1, 2]}}"), nullptr).get()));
    ASSERT_EQ(false,
              disc.isMatchCompatibleWithIndex(
                  parseMatchExpression(fromjson("{a: {$in: [1, 'abc', 2]}}"), nullptr).get()));
    ASSERT_EQ(false,
              disc.isMatchCompatibleWithIndex(
                  parseMatchExpression(fromjson("{a: {$in: [1, {b: 'abc'}, 2]}}"), nullptr).get()));
    ASSERT_EQ(
        false,
        disc.isMatchCompatibleWithIndex(
            parseMatchExpression(fromjson("{a: {$in: [1, ['abc', 'xyz'], 2]}}"), nullptr).get()));
}

// Test that a discriminator is produced for each field in a compound index (this discriminator will
// only encode collation indexability).
TEST(PlanCacheIndexabilityTest, CompoundIndexCollationDiscriminator) {
    PlanCacheIndexabilityState state;
    state.updateDiscriminators({IndexEntry(BSON("a" << 1 << "b" << 1),
                                           false,      // multikey
                                           false,      // sparse
                                           false,      // unique
                                           "a_1_b_1",  // name
                                           nullptr,
                                           BSONObj())});

    auto discriminatorsA = state.getDiscriminators("a");
    ASSERT_EQ(1U, discriminatorsA.size());
    ASSERT(discriminatorsA.find("a_1_b_1") != discriminatorsA.end());

    auto discriminatorsB = state.getDiscriminators("b");
    ASSERT_EQ(1U, discriminatorsB.size());
    ASSERT(discriminatorsB.find("a_1_b_1") != discriminatorsB.end());
}

}  // namespace
}  // namespace mongo
