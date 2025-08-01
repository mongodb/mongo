/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/plan_cache/plan_cache_indexability.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <memory>

#include <absl/container/flat_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

/**
 * Produce a MatchExpression from BSON.
 *
 * If the caller would like the MatchExpression to have a collation associated with it, they may
 * pass in an ExpressionContext owning the collation. Otherwise the caller may pass nullptr and a
 * default-constructed ExpressionContextForTest will be used.
 */
std::unique_ptr<MatchExpression> parseMatchExpression(
    const BSONObj& obj, boost::intrusive_ptr<ExpressionContext> expCtx = nullptr) {
    if (!expCtx) {
        expCtx = make_intrusive<ExpressionContextForTest>();
    }

    StatusWithMatchExpression status = MatchExpressionParser::parse(obj, std::move(expCtx));
    if (!status.isOK()) {
        FAIL(std::string(str::stream() << "failed to parse query: " << obj.toString()
                                       << ". Reason: " << status.getStatus().toString()));
    }
    return std::move(status.getValue());
}

// Helper which constructs a $** IndexEntry and returns it along with an owned ProjectionExecutor.
// The latter simulates the ProjectionExecutor which, during normal operation, is owned and
// maintained by the $** index's IndexAccessMethod, and is required because the plan cache will
// obtain unowned pointers to it.
auto makeWildcardEntry(BSONObj keyPattern, const MatchExpression* filterExpr = nullptr) {
    auto projExec = std::make_unique<WildcardProjection>(
        WildcardKeyGenerator::createProjectionExecutor(keyPattern, {}));
    return std::make_pair(IndexEntry(keyPattern,
                                     IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                                     IndexConfig::kLatestIndexVersion,
                                     false,  // multikey
                                     {},
                                     {},
                                     false,  // sparse
                                     false,  // unique
                                     IndexEntry::Identifier{"indexName"},
                                     filterExpr,
                                     BSONObj(),
                                     nullptr,
                                     projExec.get()),
                          std::move(projExec));
}

// Test sparse index discriminators for a simple sparse index.
TEST(PlanCacheIndexabilityTest, SparseIndexSimple) {
    PlanCacheIndexabilityState state;
    auto keyPattern = BSON("a" << 1);
    state.updateDiscriminators(
        {IndexEntry(keyPattern,
                    IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                    IndexConfig::kLatestIndexVersion,
                    false,  // multikey
                    {},
                    {},
                    true,                           // sparse
                    false,                          // unique
                    IndexEntry::Identifier{"a_1"},  // name
                    nullptr,                        // filterExpr
                    BSONObj(),
                    nullptr,
                    nullptr)});

    auto discriminators = state.getPathDiscriminators("a");
    ASSERT_EQ(1U, discriminators.size());
    ASSERT(discriminators.find("a_1") != discriminators.end());

    auto disc = discriminators["a_1"];
    ASSERT_EQ(true, disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("a" << 1)).get()));
    ASSERT_EQ(false,
              disc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("a" << BSONNULL)).get()));
    ASSERT_EQ(true,
              disc.isMatchCompatibleWithIndex(
                  parseMatchExpression(BSON("a" << BSON("$_internalExprEq" << 1))).get()));
    ASSERT_EQ(true,
              disc.isMatchCompatibleWithIndex(
                  parseMatchExpression(BSON("a" << BSON("$_internalExprEq" << BSONNULL))).get()));
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
    auto keyPattern = BSON("a" << 1 << "b" << 1);
    state.updateDiscriminators(
        {IndexEntry(keyPattern,
                    IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                    IndexConfig::kLatestIndexVersion,
                    false,  // multikey
                    {},
                    {},
                    true,                               // sparse
                    false,                              // unique
                    IndexEntry::Identifier{"a_1_b_1"},  // name
                    nullptr,                            // filterExpr
                    BSONObj(),
                    nullptr,
                    nullptr)});

    {
        auto discriminators = state.getPathDiscriminators("a");
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
        auto discriminators = state.getPathDiscriminators("b");
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
    auto keyPattern = BSON("a" << 1);
    state.updateDiscriminators(
        {IndexEntry(keyPattern,
                    IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                    IndexConfig::kLatestIndexVersion,
                    false,  // multikey
                    {},
                    {},
                    false,                          // sparse
                    false,                          // unique
                    IndexEntry::Identifier{"a_1"},  // name
                    filterExpr.get(),
                    BSONObj(),
                    nullptr,
                    nullptr)});

    // The partial index is represented as a global discriminator that applies to the entire
    // incoming MatchExpression.
    {
        auto discriminators = state.getPathDiscriminators("f");
        ASSERT_EQ(0U, discriminators.size());

        auto globalDiscriminators = state.getGlobalDiscriminators();
        ASSERT_EQ(1U, globalDiscriminators.size());
        ASSERT(globalDiscriminators.find("a_1") != globalDiscriminators.end());

        auto disc = globalDiscriminators["a_1"];
        ASSERT_EQ(false,
                  disc.isMatchCompatibleWithIndex(
                      parseMatchExpression(BSON("f" << BSON("$gt" << -5))).get()));
        ASSERT_EQ(true,
                  disc.isMatchCompatibleWithIndex(
                      parseMatchExpression(BSON("f" << BSON("$gt" << 5))).get()));
    }

    {
        auto discriminators = state.getPathDiscriminators("a");
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
    auto keyPattern = BSON("a" << 1);
    state.updateDiscriminators(
        {IndexEntry(keyPattern,
                    IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                    IndexConfig::kLatestIndexVersion,
                    false,  // multikey
                    {},
                    {},
                    false,                          // sparse
                    false,                          // unique
                    IndexEntry::Identifier{"a_1"},  // name
                    filterExpr.get(),
                    BSONObj(),
                    nullptr,
                    nullptr)});

    // partial index discriminators are global to the entire query, so an individual path should not
    // have any discriminators. Also the entire query must be a subset of the partial filter
    // expression, not just the leaves.
    auto globalDiscriminators = state.getGlobalDiscriminators();
    ASSERT(globalDiscriminators.find("a_1") != globalDiscriminators.end());
    auto globalDisc = globalDiscriminators["a_1"];

    {
        auto discriminators = state.getPathDiscriminators("f");
        ASSERT_EQ(0U, discriminators.size());

        ASSERT_EQ(
            false,
            globalDisc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("f" << 0)).get()));
        ASSERT_EQ(
            false,
            globalDisc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("f" << 1)).get()));
    }

    {
        auto discriminators = state.getPathDiscriminators("g");
        ASSERT_EQ(0U, discriminators.size());

        ASSERT_EQ(
            false,
            globalDisc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("g" << 0)).get()));
        ASSERT_EQ(
            false,
            globalDisc.isMatchCompatibleWithIndex(parseMatchExpression(BSON("g" << 1)).get()));
    }

    {
        // A match expression which is covered entirely by the partial filter should pass the global
        // discriminator.
        ASSERT_EQ(false,
                  globalDisc.isMatchCompatibleWithIndex(
                      parseMatchExpression(BSON("g" << 1 << "f" << 0)).get()));
        ASSERT_EQ(true,
                  globalDisc.isMatchCompatibleWithIndex(
                      parseMatchExpression(BSON("g" << 1 << "f" << 1)).get()));
    }

    {
        // The path 'a' will still have a discriminator for the collation (even though it's
        // defaulted).
        auto discriminators = state.getPathDiscriminators("a");
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
    auto keyPattern_a = BSON("a" << 1);
    auto keyPattern_b = BSON("b" << 1);
    state.updateDiscriminators(
        {IndexEntry(keyPattern_a,
                    IndexNames::nameToType(IndexNames::findPluginName(keyPattern_a)),
                    IndexConfig::kLatestIndexVersion,
                    false,  // multikey
                    {},
                    {},
                    false,                          // sparse
                    false,                          // unique
                    IndexEntry::Identifier{"a_1"},  // name
                    filterExpr1.get(),
                    BSONObj(),
                    nullptr,
                    nullptr),
         IndexEntry(keyPattern_b,
                    IndexNames::nameToType(IndexNames::findPluginName(keyPattern_b)),
                    IndexConfig::kLatestIndexVersion,
                    false,  // multikey
                    {},
                    {},
                    false,                          // sparse
                    false,                          // unique
                    IndexEntry::Identifier{"b_1"},  // name
                    filterExpr2.get(),
                    BSONObj(),
                    nullptr,
                    nullptr)});

    // partial index discriminators are global to the entire query, so an individual path within the
    // partial filter should not have any discriminators. Also the entire query must be a subset of
    // the partial filter expression, not just the leaves.
    auto globalDiscriminators = state.getGlobalDiscriminators();
    ASSERT(globalDiscriminators.find("a_1") != globalDiscriminators.end());
    ASSERT(globalDiscriminators.find("b_1") != globalDiscriminators.end());
    auto globalDiscA = globalDiscriminators["a_1"];
    auto globalDiscB = globalDiscriminators["b_1"];

    {
        auto discriminators = state.getPathDiscriminators("f");
        ASSERT_EQ(0U, discriminators.size());

        ASSERT_EQ(
            false,
            globalDiscA.isMatchCompatibleWithIndex(parseMatchExpression(BSON("f" << 0)).get()));
        ASSERT_EQ(
            false,
            globalDiscB.isMatchCompatibleWithIndex(parseMatchExpression(BSON("f" << 0)).get()));

        ASSERT_EQ(
            true,
            globalDiscA.isMatchCompatibleWithIndex(parseMatchExpression(BSON("f" << 1)).get()));
        ASSERT_EQ(
            false,
            globalDiscB.isMatchCompatibleWithIndex(parseMatchExpression(BSON("f" << 1)).get()));

        ASSERT_EQ(
            false,
            globalDiscA.isMatchCompatibleWithIndex(parseMatchExpression(BSON("f" << 2)).get()));
        ASSERT_EQ(
            true,
            globalDiscB.isMatchCompatibleWithIndex(parseMatchExpression(BSON("f" << 2)).get()));
    }

    // The paths 'a' and 'b' will have one discriminator each to capture the collation of the index.
    {
        auto discriminators = state.getPathDiscriminators("a");
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
        auto discriminators = state.getPathDiscriminators("b");
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
    auto keyPattern = BSON("a" << 1);
    state.updateDiscriminators(
        {IndexEntry(keyPattern,
                    IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                    IndexConfig::kLatestIndexVersion,
                    false,  // multikey
                    {},
                    {},
                    false,                          // sparse
                    false,                          // unique
                    IndexEntry::Identifier{"a_1"},  // name
                    nullptr,
                    BSONObj(),
                    nullptr,
                    nullptr)});
    auto discriminators = state.getPathDiscriminators("a");
    ASSERT_EQ(1U, discriminators.size());
    ASSERT(discriminators.find("a_1") != discriminators.end());
}

// Test discriminator for a simple index with a collation.
TEST(PlanCacheIndexabilityTest, DiscriminatorForCollationIndicatesWhenCollationsAreCompatible) {
    PlanCacheIndexabilityState state;
    auto keyPattern = BSON("a" << 1);
    IndexEntry entry(keyPattern,
                     IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                     IndexConfig::kLatestIndexVersion,
                     false,  // multikey
                     {},
                     {},
                     false,                          // sparse
                     false,                          // unique
                     IndexEntry::Identifier{"a_1"},  // name
                     nullptr,                        // filterExpr
                     BSONObj(),
                     nullptr,
                     nullptr);
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    entry.collator = &collator;
    state.updateDiscriminators({entry});

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(collator.clone());

    auto discriminators = state.getPathDiscriminators("a");
    ASSERT_EQ(1U, discriminators.size());
    ASSERT(discriminators.find("a_1") != discriminators.end());

    auto disc = discriminators["a_1"];

    // Index collator matches query collator.
    ASSERT_EQ(true,
              disc.isMatchCompatibleWithIndex(
                  parseMatchExpression(fromjson("{a: 'abc'}"), expCtx).get()));
    ASSERT_EQ(true,
              disc.isMatchCompatibleWithIndex(
                  parseMatchExpression(fromjson("{a: {$in: ['abc', 'xyz']}}"), expCtx).get()));
    ASSERT_EQ(true,
              disc.isMatchCompatibleWithIndex(
                  parseMatchExpression(fromjson("{a: {$_internalExprEq: 'abc'}}"), expCtx).get()));

    // Expression is not a ComparisonMatchExpression, InternalExprEqMatchExpression or
    // InMatchExpression.
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

    // Expression is an InternalExprEqMatchExpression with non-matching collator.
    ASSERT_EQ(true,
              disc.isMatchCompatibleWithIndex(
                  parseMatchExpression(fromjson("{a: {$_internalExprEq:  5}}"), nullptr).get()));
    ASSERT_EQ(false,
              disc.isMatchCompatibleWithIndex(
                  parseMatchExpression(fromjson("{a: {$_internalExprEq: 'abc'}}"), nullptr).get()));
    ASSERT_EQ(
        false,
        disc.isMatchCompatibleWithIndex(
            parseMatchExpression(fromjson("{a: {$_internalExprEq: {b: 'abc'}}}"), nullptr).get()));

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
    auto keyPattern = BSON("a" << 1 << "b" << 1);
    state.updateDiscriminators(
        {IndexEntry(keyPattern,
                    IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                    IndexConfig::kLatestIndexVersion,
                    false,  // multikey
                    {},
                    {},
                    false,                              // sparse
                    false,                              // unique
                    IndexEntry::Identifier{"a_1_b_1"},  // name
                    nullptr,
                    BSONObj(),
                    nullptr,
                    nullptr)});

    auto discriminatorsA = state.getPathDiscriminators("a");
    ASSERT_EQ(1U, discriminatorsA.size());
    ASSERT(discriminatorsA.find("a_1_b_1") != discriminatorsA.end());

    auto discriminatorsB = state.getPathDiscriminators("b");
    ASSERT_EQ(1U, discriminatorsB.size());
    ASSERT(discriminatorsB.find("a_1_b_1") != discriminatorsB.end());
}

TEST(PlanCacheIndexabilityTest, WildcardDiscriminator) {
    PlanCacheIndexabilityState state;
    auto entryProjExecPair = makeWildcardEntry(BSON("a.$**" << 1));
    state.updateDiscriminators({entryProjExecPair.first});

    const auto unindexedPathDiscriminators = state.buildWildcardDiscriminators("notIndexed");
    ASSERT_EQ(0U, unindexedPathDiscriminators.size());

    auto discriminatorsA = state.buildWildcardDiscriminators("a");
    ASSERT_EQ(1U, discriminatorsA.size());
    ASSERT(discriminatorsA.find("indexName") != discriminatorsA.end());

    const auto disc = discriminatorsA["indexName"];

    ASSERT_TRUE(
        disc.isMatchCompatibleWithIndex(parseMatchExpression(fromjson("{a: 'abc'}")).get()));

    // Querying for array values isn't supported by wildcard indexes.
    ASSERT_FALSE(
        disc.isMatchCompatibleWithIndex(parseMatchExpression(fromjson("{a: [1, 2, 3]}")).get()));
    // Querying for null isn't supported by wildcard indexes.
    ASSERT_FALSE(
        disc.isMatchCompatibleWithIndex(parseMatchExpression(fromjson("{a: null}")).get()));

    // Equality on empty array is supported.
    ASSERT_TRUE(disc.isMatchCompatibleWithIndex(parseMatchExpression(fromjson("{a: []}")).get()));
    // Inequality isn't.
    ASSERT_FALSE(
        disc.isMatchCompatibleWithIndex(parseMatchExpression(fromjson("{a: {$gt: []}}")).get()));

    // Cases which use $in.
    ASSERT_TRUE(
        disc.isMatchCompatibleWithIndex(parseMatchExpression(fromjson("{a: {$in: []}}")).get()));
    ASSERT_TRUE(disc.isMatchCompatibleWithIndex(
        parseMatchExpression(fromjson("{a: {$in: [1, 2, 's']}}")).get()));
    // Empty array inside the $in.
    ASSERT_TRUE(disc.isMatchCompatibleWithIndex(
        parseMatchExpression(fromjson("{a: {$in: [1, [], 's']}}")).get()));

    // Objects, non-empty arrays and null inside a $in.
    ASSERT_FALSE(disc.isMatchCompatibleWithIndex(
        parseMatchExpression(fromjson("{a: {$in: [1, {a: 1}, 's']}}")).get()));
    ASSERT_FALSE(disc.isMatchCompatibleWithIndex(
        parseMatchExpression(fromjson("{a: {$in: [1, [1,2,3], 's']}}")).get()));
    ASSERT_FALSE(disc.isMatchCompatibleWithIndex(
        parseMatchExpression(fromjson("{a: {$in: [1, 2, null]}}")).get()));
}

TEST(PlanCacheIndexabilityTest, WildcardWithCollationDiscriminator) {
    PlanCacheIndexabilityState state;
    auto entryProjExecPair = makeWildcardEntry(BSON("a.$**" << 1));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    entryProjExecPair.first.collator = &collator;

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(collator.clone());

    state.updateDiscriminators({entryProjExecPair.first});

    const auto unindexedPathDiscriminators = state.buildWildcardDiscriminators("notIndexed");
    ASSERT_EQ(0U, unindexedPathDiscriminators.size());

    auto discriminatorsA = state.buildWildcardDiscriminators("a");
    ASSERT_EQ(1U, discriminatorsA.size());
    ASSERT(discriminatorsA.find("indexName") != discriminatorsA.end());

    const auto disc = discriminatorsA["indexName"];

    // Match expression which uses the simple collation isn't compatible.
    ASSERT_FALSE(disc.isMatchCompatibleWithIndex(
        parseMatchExpression(fromjson("{a: \"hello world\"}"), nullptr).get()));
    // Match expression which uses the same collation as the index is.
    ASSERT_TRUE(disc.isMatchCompatibleWithIndex(
        parseMatchExpression(fromjson("{a: \"hello world\"}"), expCtx).get()));
}

TEST(PlanCacheIndexabilityTest, WildcardPartialIndexDiscriminator) {
    PlanCacheIndexabilityState state;

    // Need to keep the filter BSON object around for the duration of the test since the match
    // expression will store (unowned) pointers into it.
    BSONObj filterObj = fromjson("{a: {$gt: 5}}");
    auto filterExpr = parseMatchExpression(filterObj);
    auto entryProjExecPair = makeWildcardEntry(BSON("$**" << 1), filterExpr.get());
    state.updateDiscriminators({entryProjExecPair.first});

    auto discriminatorsA = state.buildWildcardDiscriminators("a");
    ASSERT_EQ(1U, discriminatorsA.size());
    ASSERT(discriminatorsA.find("indexName") != discriminatorsA.end());

    const auto wildcardDiscriminators = discriminatorsA["indexName"];

    // Since the fields in the partialFilterExpression are known a priori, they are _not_ part of
    // the wildcard-discriminators, but rather the regular discriminators. Here we show that the
    // wildcard discriminators consider all expressions on fields 'a' or 'b' to be compatible.
    ASSERT_TRUE(wildcardDiscriminators.isMatchCompatibleWithIndex(
        parseMatchExpression(fromjson("{a: 0}")).get()));
    ASSERT_TRUE(wildcardDiscriminators.isMatchCompatibleWithIndex(
        parseMatchExpression(fromjson("{a: 6}")).get()));
    ASSERT_TRUE(wildcardDiscriminators.isMatchCompatibleWithIndex(
        parseMatchExpression(fromjson("{b: 0}")).get()));
    ASSERT_TRUE(wildcardDiscriminators.isMatchCompatibleWithIndex(
        parseMatchExpression(fromjson("{b: 6}")).get()));

    // The global discriminator for the index "indexName" should reflect whether a MatchExpression
    // is compatible with the partial filter expression.
    {
        discriminatorsA = state.getPathDiscriminators("a");
        ASSERT(discriminatorsA.find("indexName") == discriminatorsA.end());

        auto globalDisc = state.getGlobalDiscriminators();
        ASSERT(globalDisc.find("indexName") != globalDisc.end());
        auto disc = globalDisc["indexName"];

        ASSERT_FALSE(
            disc.isMatchCompatibleWithIndex(parseMatchExpression(fromjson("{a: 0}")).get()));
        ASSERT_TRUE(
            disc.isMatchCompatibleWithIndex(parseMatchExpression(fromjson("{a: 6}")).get()));

        ASSERT_FALSE(
            disc.isMatchCompatibleWithIndex(parseMatchExpression(fromjson("{b: 0}")).get()));
        ASSERT_FALSE(
            disc.isMatchCompatibleWithIndex(parseMatchExpression(fromjson("{b: 6}")).get()));
    }

    // There shouldn't be any regular discriminators associated with path "b".
    {
        auto&& discriminatorsB = state.getPathDiscriminators("b");
        ASSERT_FALSE(discriminatorsB.count("indexName"));
    }
}

TEST(PlanCacheIndexabilityTest,
     WildcardIndexDiscriminatesBetweenEqualityToEmptyObjAndOtherObjComparisons) {
    PlanCacheIndexabilityState state;
    auto entryProjExecPair = makeWildcardEntry(BSON("a.$**" << 1));
    state.updateDiscriminators({entryProjExecPair.first});

    auto discriminatorsA = state.buildWildcardDiscriminators("a");
    ASSERT_EQ(1U, discriminatorsA.size());
    ASSERT(discriminatorsA.find("indexName") != discriminatorsA.end());

    const auto disc = discriminatorsA["indexName"];

    // Verify that the discriminator considers equality to empty object compatible.
    ASSERT_TRUE(disc.isMatchCompatibleWithIndex(parseMatchExpression(fromjson("{a: {}}")).get()));

    // $lte:{} is a synonym for $eq:{}, and therefore is also compatible.
    ASSERT_TRUE(
        disc.isMatchCompatibleWithIndex(parseMatchExpression(fromjson("{a: {$lte: {}}}")).get()));

    // An $in with an empty object is compatible.
    ASSERT_TRUE(disc.isMatchCompatibleWithIndex(
        parseMatchExpression(fromjson("{a: {$in: [1, {}, 's']}}")).get()));

    // Equality to a non-empty object is not compatible.
    ASSERT_FALSE(
        disc.isMatchCompatibleWithIndex(parseMatchExpression(fromjson("{a: {b: 1}}")).get()));

    // Inequality with an empty object is not compatible.
    ASSERT_FALSE(
        disc.isMatchCompatibleWithIndex(parseMatchExpression(fromjson("{a: {$lt: {}}}")).get()));
    ASSERT_FALSE(
        disc.isMatchCompatibleWithIndex(parseMatchExpression(fromjson("{a: {$gt: {}}}")).get()));
    ASSERT_FALSE(
        disc.isMatchCompatibleWithIndex(parseMatchExpression(fromjson("{a: {$gte: {}}}")).get()));

    // Inequality with a non-empty object is not compatible.
    ASSERT_FALSE(disc.isMatchCompatibleWithIndex(
        parseMatchExpression(fromjson("{a: {$gte: {b: 1}}}")).get()));

    // An $in with a non-empty object is not compatible.
    ASSERT_FALSE(disc.isMatchCompatibleWithIndex(
        parseMatchExpression(fromjson("{a: {$in: [1, {a: 1}, 's']}}")).get()));
}

}  // namespace
}  // namespace mongo
