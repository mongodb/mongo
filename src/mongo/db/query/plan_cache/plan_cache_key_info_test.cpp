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

#include "mongo/db/query/plan_cache/plan_cache_key_info.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_query_test_util.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache_indexability.h"
#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#include <iosfwd>
#include <memory>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using std::string;
using std::unique_ptr;
using std::vector;


std::ostream& operator<<(std::ostream& stream, const PlanCacheKeyInfo& key) {
    stream << key.toString() << " settings: " << key.querySettings().toBSON().toString();
    return stream;
}

auto makeDbName(StringData dbName) {
    return DatabaseNameUtil::deserialize(
        boost::none /*tenantId=*/, dbName, SerializationContext::stateDefault());
}


namespace {
using PlanCacheKeyInfoTest = CanonicalQueryTest;

PlanCacheKeyInfo makeKey(const CanonicalQuery& cq,
                         const std::vector<CoreIndexInfo>& indexCores = {},
                         const query_settings::QuerySettings& settings = {}) {
    PlanCacheIndexabilityState indexabilityState;
    indexabilityState.updateDiscriminators(indexCores);

    StringBuilder indexabilityKeyBuilder;
    plan_cache_detail::encodeIndexability(
        cq.getPrimaryMatchExpression(), indexabilityState, &indexabilityKeyBuilder);

    return {encodeKey(cq), indexabilityKeyBuilder.str(), settings};
}

/**
 * Utility function to create MatchExpression
 */
unique_ptr<MatchExpression> parseMatchExpression(const BSONObj& obj) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(obj,
                                     std::move(expCtx),
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    if (!status.isOK()) {
        str::stream ss;
        ss << "failed to parse query: " << obj.toString()
           << ". Reason: " << status.getStatus().toString();
        FAIL(std::string(ss));
    }

    return std::move(status.getValue());
}


// A version of the above for CoreIndexInfo, used for plan cache update tests.
std::pair<CoreIndexInfo, std::unique_ptr<WildcardProjection>> makeWildcardUpdate(
    BSONObj keyPattern) {
    auto wcProj = std::make_unique<WildcardProjection>(
        WildcardKeyGenerator::createProjectionExecutor(keyPattern, {}));
    return {CoreIndexInfo(keyPattern,
                          IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                          false,                                // sparse
                          IndexEntry::Identifier{"indexName"},  // name
                          nullptr,                              // filterExpr
                          nullptr,                              // collation
                          wcProj.get()),                        // wildcard
            std::move(wcProj)};
}

/**
 * Check that the stable keys of 'a' and 'b' are equal, but the index discriminators are not.
 */
void assertPlanCacheKeysUnequalDueToDiscriminators(const PlanCacheKeyInfo& a,
                                                   const PlanCacheKeyInfo& b) {
    ASSERT_EQ(a.getQueryShapeStringData(), b.getQueryShapeStringData());
    ASSERT_NE(a.getIndexabilityDiscriminators(), b.getIndexabilityDiscriminators());

    // Should always have the begin and end delimiters.
    ASSERT_GTE(a.getIndexabilityDiscriminators().size(), 2u);
}

}  // namespace

TEST_F(PlanCacheKeyInfoTest, EqualityOperator) {
    unique_ptr<CanonicalQuery> cqEqZero(canonicalize("{a: 0}"));
    unique_ptr<CanonicalQuery> cqEqOne(canonicalize("{a: 1}"));

    NamespaceSpec nsSpec;
    nsSpec.setDb(makeDbName("db"));
    nsSpec.setColl("coll"_sd);
    query_settings::QuerySettings settingsA1IndexKeyPattern;
    settingsA1IndexKeyPattern.setIndexHints(
        {{query_settings::IndexHintSpec(nsSpec, {IndexHint(BSON("a" << 1))})}});
    query_settings::QuerySettings settingsA1IndexName;
    settingsA1IndexName.setIndexHints(
        {{query_settings::IndexHintSpec(nsSpec, {IndexHint("a_1")})}});
    query_settings::QuerySettings settingsB1IndexKeyPattern;
    settingsB1IndexKeyPattern.setIndexHints(
        {{query_settings::IndexHintSpec(nsSpec, {IndexHint(BSON("b" << 1))})}});

    auto keyWithoutSettingsZero = makeKey(*cqEqZero, {} /*indexCores=*/);
    auto keyWithSettingsZeroA1IndexKeyPattern =
        makeKey(*cqEqZero, {} /*indexCores=*/, settingsA1IndexKeyPattern);
    auto keyWithSettingsZeroA1IndexName =
        makeKey(*cqEqZero, {} /*indexCores=*/, settingsA1IndexName);
    auto keyWithSettingsOneA1IndexKeyPattern =
        makeKey(*cqEqOne, {} /*indexCores=*/, settingsA1IndexKeyPattern);
    auto keyWithSettingsOneA1IndexName = makeKey(*cqEqOne, {} /*indexCores=*/, settingsA1IndexName);
    auto keyWithSettingsOneB1IndexKeyPattern =
        makeKey(*cqEqOne, {} /*indexCores=*/, settingsB1IndexKeyPattern);

    // Ensure that presence of query settings results in different hash and makes equality fail.
    ASSERT_NE(keyWithoutSettingsZero.planCacheKeyHash(),
              keyWithSettingsZeroA1IndexKeyPattern.planCacheKeyHash());
    ASSERT_NE(keyWithoutSettingsZero, keyWithSettingsZeroA1IndexKeyPattern);

    ASSERT_NE(keyWithoutSettingsZero.planCacheKeyHash(),
              keyWithSettingsOneA1IndexKeyPattern.planCacheKeyHash());
    ASSERT_NE(keyWithoutSettingsZero, keyWithSettingsOneA1IndexKeyPattern);

    // Ensure that keys with same query settings and equivalent 'planCacheShapeHash' results in
    // same hash and passes equality check.
    ASSERT_EQ(keyWithSettingsOneA1IndexName.planCacheKeyHash(),
              keyWithSettingsZeroA1IndexName.planCacheKeyHash());
    ASSERT_EQ(keyWithSettingsOneA1IndexName, keyWithSettingsZeroA1IndexName);

    ASSERT_EQ(keyWithSettingsOneA1IndexKeyPattern.planCacheKeyHash(),
              keyWithSettingsZeroA1IndexKeyPattern.planCacheKeyHash());
    ASSERT_EQ(keyWithSettingsOneA1IndexKeyPattern, keyWithSettingsZeroA1IndexKeyPattern);

    // Ensure that if settings are different, both hashes are different and keys are considered
    // unequal.
    ASSERT_NE(keyWithSettingsOneA1IndexKeyPattern.planCacheKeyHash(),
              keyWithSettingsZeroA1IndexName.planCacheKeyHash());
    ASSERT_NE(keyWithSettingsOneA1IndexKeyPattern, keyWithSettingsZeroA1IndexName);

    ASSERT_NE(keyWithSettingsOneA1IndexKeyPattern.planCacheKeyHash(),
              keyWithSettingsOneA1IndexName.planCacheKeyHash());
    ASSERT_NE(keyWithSettingsOneA1IndexKeyPattern, keyWithSettingsOneA1IndexName);

    ASSERT_NE(keyWithSettingsOneA1IndexKeyPattern.planCacheKeyHash(),
              keyWithSettingsOneB1IndexKeyPattern.planCacheKeyHash());
    ASSERT_NE(keyWithSettingsOneA1IndexKeyPattern, keyWithSettingsOneB1IndexKeyPattern);
}

// When a sparse index is present, computeKey() should generate different keys depending on
// whether or not the predicates in the given query can use the index.
TEST_F(PlanCacheKeyInfoTest, ComputeKeySparseIndex) {
    const auto keyPattern = BSON("a" << 1);
    const std::vector<CoreIndexInfo> indexCores = {
        CoreIndexInfo(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                      true,                          // sparse
                      IndexEntry::Identifier{""})};  // name

    unique_ptr<CanonicalQuery> cqEqNumber(canonicalize("{a: 0}"));
    unique_ptr<CanonicalQuery> cqEqString(canonicalize("{a: 'x'}"));
    unique_ptr<CanonicalQuery> cqEqNull(canonicalize("{a: null}"));

    // 'cqEqNumber' and 'cqEqString' get the same key, since both are compatible with this
    // index.
    const auto eqNumberKey = makeKey(*cqEqNumber, indexCores);
    const auto eqStringKey = makeKey(*cqEqString, indexCores);
    ASSERT_EQ(eqNumberKey, eqStringKey);

    // 'cqEqNull' gets a different key, since it is not compatible with this index.
    const auto eqNullKey = makeKey(*cqEqNull, indexCores);
    ASSERT_NOT_EQUALS(eqNullKey, eqNumberKey);

    assertPlanCacheKeysUnequalDueToDiscriminators(eqNullKey, eqNumberKey);
    assertPlanCacheKeysUnequalDueToDiscriminators(eqNullKey, eqStringKey);
}

// When a partial index is present, computeKey() should generate different keys depending on
// whether or not the predicates in the given query "match" the predicates in the partial index
// filter.
TEST_F(PlanCacheKeyInfoTest, ComputeKeyPartialIndex) {
    BSONObj filterObj = BSON("f" << BSON("$gt" << 0));
    unique_ptr<MatchExpression> filterExpr(parseMatchExpression(filterObj));

    const auto keyPattern = BSON("a" << 1);
    const std::vector<CoreIndexInfo> indexCores = {
        CoreIndexInfo(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                      false,                       // sparse
                      IndexEntry::Identifier{""},  // name
                      filterExpr.get())};          // filterExpr

    unique_ptr<CanonicalQuery> cqGtNegativeFive(canonicalize("{f: {$gt: -5}}"));
    unique_ptr<CanonicalQuery> cqGtZero(canonicalize("{f: {$gt: 0}}"));
    unique_ptr<CanonicalQuery> cqGtFive(canonicalize("{f: {$gt: 5}}"));

    // 'cqGtZero' and 'cqGtFive' get the same key, since both are compatible with this index.
    ASSERT_EQ(makeKey(*cqGtZero, indexCores), makeKey(*cqGtFive, indexCores));

    // 'cqGtNegativeFive' gets a different key, since it is not compatible with this index.
    assertPlanCacheKeysUnequalDueToDiscriminators(makeKey(*cqGtNegativeFive, indexCores),
                                                  makeKey(*cqGtZero, indexCores));
}

TEST_F(PlanCacheKeyInfoTest, ComputeKeyPartialIndexConjunction) {
    BSONObj filterObj = fromjson("{f: {$gt: 0, $lt: 10}}");
    unique_ptr<MatchExpression> filterExpr(parseMatchExpression(filterObj));

    const auto keyPattern = BSON("a" << 1);
    const std::vector<CoreIndexInfo> indexCores = {
        CoreIndexInfo(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                      false,                       // sparse
                      IndexEntry::Identifier{""},  // name
                      filterExpr.get())};          // filterExpr

    unique_ptr<CanonicalQuery> satisfySinglePredicate(canonicalize("{f: {$gt: 0}}"));
    ASSERT_EQ(makeKey(*satisfySinglePredicate, indexCores).getIndexabilityDiscriminators(), "(0)");

    unique_ptr<CanonicalQuery> satisfyBothPredicates(canonicalize("{f: {$eq: 5}}"));
    ASSERT_EQ(makeKey(*satisfyBothPredicates, indexCores).getIndexabilityDiscriminators(), "(1)");

    unique_ptr<CanonicalQuery> conjSingleField(canonicalize("{f: {$gt: 2, $lt: 9}}"));
    ASSERT_EQ(makeKey(*conjSingleField, indexCores).getIndexabilityDiscriminators(), "(1)");

    unique_ptr<CanonicalQuery> conjSingleFieldNoMatch(canonicalize("{f: {$gt: 2, $lt: 11}}"));
    ASSERT_EQ(makeKey(*conjSingleFieldNoMatch, indexCores).getIndexabilityDiscriminators(),
              "(000)");

    // Note that these queries get optimized to a single $in over 'f'.
    unique_ptr<CanonicalQuery> disjSingleFieldBothSatisfy(
        canonicalize("{$or: [{f: {$eq: 2}}, {f: {$eq: 3}}]}"));
    ASSERT_EQ(makeKey(*disjSingleFieldBothSatisfy, indexCores).getIndexabilityDiscriminators(),
              "(1)");

    unique_ptr<CanonicalQuery> disjSingleFieldNotSubset(
        canonicalize("{$or: [{f: {$eq: 2}}, {f: {$eq: 11}}]}"));
    ASSERT_EQ(makeKey(*disjSingleFieldNotSubset, indexCores).getIndexabilityDiscriminators(),
              "(0)");
}

TEST_F(PlanCacheKeyInfoTest, ComputeKeyPartialIndexDisjunction) {
    BSONObj filterObj = fromjson("{$or: [{f: {$gt: 10}}, {f: {$lt: 0}}]}");
    unique_ptr<MatchExpression> filterExpr(parseMatchExpression(filterObj));

    const auto keyPattern = BSON("a" << 1);
    const std::vector<CoreIndexInfo> indexCores = {
        CoreIndexInfo(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                      false,                       // sparse
                      IndexEntry::Identifier{""},  // name
                      filterExpr.get())};          // filterExpr

    unique_ptr<CanonicalQuery> satisfySinglePredicate(canonicalize("{f: {$eq: 11}}"));
    ASSERT_EQ(makeKey(*satisfySinglePredicate, indexCores).getIndexabilityDiscriminators(), "(1)");

    unique_ptr<CanonicalQuery> satisfyNeither(canonicalize("{f: {$eq: 5}}"));
    ASSERT_EQ(makeKey(*satisfyNeither, indexCores).getIndexabilityDiscriminators(), "(0)");

    unique_ptr<CanonicalQuery> conjSingleFieldMatch(canonicalize("{f: {$lt: 20, $gt: 10}}"));
    ASSERT_EQ(makeKey(*conjSingleFieldMatch, indexCores).getIndexabilityDiscriminators(), "(1)");

    unique_ptr<CanonicalQuery> conjSingleFieldNoMatch(canonicalize("{f: {$gt: 2, $lt: 10}}"));
    ASSERT_EQ(makeKey(*conjSingleFieldNoMatch, indexCores).getIndexabilityDiscriminators(),
              "(000)");

    unique_ptr<CanonicalQuery> conjSingleFieldOverlap(canonicalize("{f: {$gt: 2, $lt: 12}}"));
    ASSERT_EQ(makeKey(*conjSingleFieldOverlap, indexCores).getIndexabilityDiscriminators(),
              "(000)");

    unique_ptr<CanonicalQuery> disjSingleFieldBothSatisfy(
        canonicalize("{$or: [{f: {$eq: -1}}, {f: {$gt: 10}}]}"));
    ASSERT_EQ(makeKey(*disjSingleFieldBothSatisfy, indexCores).getIndexabilityDiscriminators(),
              "(1)");

    // Gets rewritten to {f: {$in: [2, 11]}}
    unique_ptr<CanonicalQuery> disjSingleFieldNotSubset(
        canonicalize("{$or: [{f: {$eq: 2}}, {f: {$eq: 11}}]}"));
    ASSERT_EQ(makeKey(*disjSingleFieldNotSubset, indexCores).getIndexabilityDiscriminators(),
              "(0)");

    unique_ptr<CanonicalQuery> disjSingleFieldOneDisjunctSatisfies(
        canonicalize("{$or: [{f: {$eq: 2}}, {f: {$gt: 10}}]}"));
    ASSERT_EQ(
        makeKey(*disjSingleFieldOneDisjunctSatisfies, indexCores).getIndexabilityDiscriminators(),
        "(001)");
}

TEST_F(PlanCacheKeyInfoTest, PartialIndexQueryElemMatch) {
    auto filterObj = fromjson("{$or: [{'f.a': 1}, {'f.a': {$gt: 2}}]}");
    unique_ptr<MatchExpression> filterExpr(parseMatchExpression(filterObj));

    const auto keyPattern = BSON("a" << 1);
    const std::vector<CoreIndexInfo> indexCores = {
        CoreIndexInfo(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                      false,                       // sparse
                      IndexEntry::Identifier{""},  // name
                      filterExpr.get())};          // filterExpr

    {
        unique_ptr<CanonicalQuery> nonElemMatch(
            canonicalize("{$or: [{'f.a': 1}, {'f.a': {$gt: 2}}]}"));
        ASSERT_EQ(makeKey(*nonElemMatch, indexCores).getIndexabilityDiscriminators(), "(1)");
    }

    {
        unique_ptr<CanonicalQuery> nonElemMatchSingleMatch(
            canonicalize("{$or: [{'f.a': 1}, {'f.a': {$gt: 0}}]}"));
        ASSERT_EQ(makeKey(*nonElemMatchSingleMatch, indexCores).getIndexabilityDiscriminators(),
                  "(010)");
    }

    {
        // Verify that we don't mark $elemMatch as eligible even when its children are identical to
        // the filter.
        unique_ptr<CanonicalQuery> elemMatch(
            canonicalize("{f: {$elemMatch: {$or: [{a: 1}, {a: {$gt: 2}}]}}}"));
        ASSERT_EQ(makeKey(*elemMatch, indexCores).getIndexabilityDiscriminators(), "(0000)");
    }
}

TEST_F(PlanCacheKeyInfoTest, PartialIndexQueryNegation) {
    auto filterObj = fromjson("{a: 1}");
    unique_ptr<MatchExpression> filterExpr(parseMatchExpression(filterObj));

    const auto keyPattern = BSON("a" << 1);
    const std::vector<CoreIndexInfo> indexCores = {
        CoreIndexInfo(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                      false,                       // sparse
                      IndexEntry::Identifier{""},  // name
                      filterExpr.get())};          // filterExpr

    {
        unique_ptr<CanonicalQuery> nor(canonicalize("{$nor: [{a: 1}]}"));
        ASSERT_EQ(makeKey(*nor, indexCores).getIndexabilityDiscriminators(), "(00)<1><1>");
    }
    {
        unique_ptr<CanonicalQuery> notcq(canonicalize("{a: {$ne: 1}}"));
        ASSERT_EQ(makeKey(*notcq, indexCores).getIndexabilityDiscriminators(), "(00)<1><1>");
    }
}

TEST_F(PlanCacheKeyInfoTest, ComputeKeyPartialIndexNestedDisjunction) {
    BSONObj filterObj = fromjson(R"(
        {$and: [
            {$or: [{f: {$gt: 10}}, {f: {$lt: 0}}]}, 
            {$or: [{f: {$gt: 11}}, {f: {$lt: 1}}]} 
        ]})");
    unique_ptr<MatchExpression> filterExpr(parseMatchExpression(filterObj));

    const auto keyPattern = BSON("a" << 1);
    const std::vector<CoreIndexInfo> indexCores = {
        CoreIndexInfo(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                      false,                       // sparse
                      IndexEntry::Identifier{""},  // name
                      filterExpr.get())};          // filterExpr


    unique_ptr<CanonicalQuery> satisfySinglePredicate(canonicalize("{f: {$eq: 11}}"));
    ASSERT_EQ(makeKey(*satisfySinglePredicate, indexCores).getIndexabilityDiscriminators(), "(0)");

    unique_ptr<CanonicalQuery> notCompat(canonicalize("{f: {$eq: 12}}"));
    ASSERT_EQ(makeKey(*notCompat, indexCores).getIndexabilityDiscriminators(), "(1)");
}

// Query shapes should get the same plan cache key if they have the same collation indexability.
TEST_F(PlanCacheKeyInfoTest, ComputeKeyCollationIndex) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);

    const auto keyPattern = BSON("a" << 1);
    const std::vector<CoreIndexInfo> indexCores = {
        CoreIndexInfo(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                      false,                       // sparse
                      IndexEntry::Identifier{""},  // name
                      nullptr,                     // filterExpr
                      &collator)};                 // collation

    unique_ptr<CanonicalQuery> containsString(canonicalize("{a: 'abc'}"));
    unique_ptr<CanonicalQuery> containsObject(canonicalize("{a: {b: 'abc'}}"));
    unique_ptr<CanonicalQuery> containsArray(canonicalize("{a: ['abc', 'xyz']}"));
    unique_ptr<CanonicalQuery> noStrings(canonicalize("{a: 5}"));
    unique_ptr<CanonicalQuery> containsStringHasCollation(
        canonicalize("{a: 'abc'}", "{}", "{}", "{locale: 'mock_reverse_string'}"));

    // 'containsString', 'containsObject', and 'containsArray' have the same key, since none are
    // compatible with the index.
    ASSERT_EQ(makeKey(*containsString, indexCores), makeKey(*containsObject, indexCores));
    ASSERT_EQ(makeKey(*containsString, indexCores), makeKey(*containsArray, indexCores));

    // 'noStrings' gets a different key since it is compatible with the index.
    assertPlanCacheKeysUnequalDueToDiscriminators(makeKey(*containsString, indexCores),
                                                  makeKey(*noStrings, indexCores));
    ASSERT_EQ(makeKey(*containsString, indexCores).getIndexabilityDiscriminators(), "<0>");
    ASSERT_EQ(makeKey(*noStrings, indexCores).getIndexabilityDiscriminators(), "<1>");

    // 'noStrings' and 'containsStringHasCollation' get different keys, since the collation
    // specified in the query is considered part of its shape. However, they have the same index
    // compatibility, so the unstable part of their PlanCacheKeys should be the same.
    auto noStringKey = makeKey(*noStrings, indexCores);
    auto withStringAndCollationKey = makeKey(*containsStringHasCollation, indexCores);
    ASSERT_NE(noStringKey, withStringAndCollationKey);
    ASSERT_EQ(noStringKey.getIndexabilityDiscriminators(),
              withStringAndCollationKey.getIndexabilityDiscriminators());
    ASSERT_NE(noStringKey.getQueryShapeStringData(),
              withStringAndCollationKey.getQueryShapeStringData());

    unique_ptr<CanonicalQuery> inContainsString(canonicalize("{a: {$in: [1, 'abc', 2]}}"));
    unique_ptr<CanonicalQuery> inContainsObject(canonicalize("{a: {$in: [1, {b: 'abc'}, 2]}}"));
    unique_ptr<CanonicalQuery> inContainsArray(canonicalize("{a: {$in: [1, ['abc', 'xyz'], 2]}}"));
    unique_ptr<CanonicalQuery> inNoStrings(canonicalize("{a: {$in: [1, 2]}}"));
    unique_ptr<CanonicalQuery> inContainsStringHasCollation(
        canonicalize("{a: {$in: [1, 'abc', 2]}}", "{}", "{}", "{locale: 'mock_reverse_string'}"));

    // 'inContainsString', 'inContainsObject', and 'inContainsArray' have the same key, since none
    // are compatible with the index.
    ASSERT_EQ(makeKey(*inContainsString, indexCores), makeKey(*inContainsObject, indexCores));
    ASSERT_EQ(makeKey(*inContainsString, indexCores), makeKey(*inContainsArray, indexCores));

    // 'inNoStrings' gets a different key since it is compatible with the index.
    assertPlanCacheKeysUnequalDueToDiscriminators(makeKey(*inContainsString, indexCores),
                                                  makeKey(*inNoStrings, indexCores));
    ASSERT_EQ(makeKey(*inContainsString, indexCores).getIndexabilityDiscriminators(), "<0>");
    ASSERT_EQ(makeKey(*inNoStrings, indexCores).getIndexabilityDiscriminators(), "<1>");

    // 'inNoStrings' and 'inContainsStringHasCollation' get the same key since they compatible with
    // the index.
    ASSERT_NE(makeKey(*inNoStrings, indexCores),
              makeKey(*inContainsStringHasCollation, indexCores));
    ASSERT_EQ(makeKey(*inNoStrings, indexCores).getIndexabilityDiscriminators(),
              makeKey(*inContainsStringHasCollation, indexCores).getIndexabilityDiscriminators());

    unique_ptr<CanonicalQuery> elemMatchString(canonicalize("{a: {$elemMatch: {$eq: 'abc'}}}"));
    unique_ptr<CanonicalQuery> elemMatchNoString(canonicalize("{a: {$elemMatch: {$eq: 5}}}"));

    // 'elemMatchString' and 'elemMatchNoString' should have different discriminators.
    assertPlanCacheKeysUnequalDueToDiscriminators(makeKey(*elemMatchString, indexCores),
                                                  makeKey(*elemMatchNoString, indexCores));

    unique_ptr<CanonicalQuery> elemMatchAndFirstBoundString(
        canonicalize("{a: {$elemMatch: {$gt: 'A', $lt: 5}}}"));
    unique_ptr<CanonicalQuery> elemMatchAndSecondBoundString(
        canonicalize("{a: {$elemMatch: {$gt: 1, $lt: 'B'}}}"));
    unique_ptr<CanonicalQuery> elemMatchAndNoString(
        canonicalize("{a: {$elemMatch: {$gt: 1, $lt: 5}}}"));

    // 'elemMatchAndFirstBoundString'/'elemMatchAndSecondBoundString' and 'elemMatchAndNoString'
    // should have different discriminators.
    assertPlanCacheKeysUnequalDueToDiscriminators(
        makeKey(*elemMatchAndFirstBoundString, indexCores),
        makeKey(*elemMatchAndNoString, indexCores));
    assertPlanCacheKeysUnequalDueToDiscriminators(
        makeKey(*elemMatchAndSecondBoundString, indexCores),
        makeKey(*elemMatchAndNoString, indexCores));
}

TEST_F(PlanCacheKeyInfoTest, ComputeKeyElemMatchNoIndexOnPath) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    const auto keyPattern = BSON("a" << 1);
    const std::vector<CoreIndexInfo> indexCores = {
        CoreIndexInfo(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                      false,                       // sparse
                      IndexEntry::Identifier{""},  // name
                      nullptr,                     // filterExpr
                      &collator)};                 // collation

    // $elemMatch on "b.a" cannot use the index on field "a"; it should recognize that the field "a"
    // being referred to in the query is not top-level.
    unique_ptr<CanonicalQuery> valueElemMatchOnDiffField(
        canonicalize("{b: {$elemMatch: {a: {$eq: 5}}}}"));
    ASSERT_EQ(makeKey(*valueElemMatchOnDiffField, indexCores).getIndexabilityDiscriminators(), "");
}

TEST_F(PlanCacheKeyInfoTest, ComputeKeyObjectElemMatch) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    const auto keyPattern = BSON("a.b" << 1);
    const std::vector<CoreIndexInfo> indexCores = {
        CoreIndexInfo(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                      false,                       // sparse
                      IndexEntry::Identifier{""},  // name
                      nullptr,                     // filterExpr
                      &collator)};                 // collation

    // Object elemMatch on "a.b" should detect string comparison when query collation differs.
    unique_ptr<CanonicalQuery> objElemMatchNoString(
        canonicalize("{a: {$elemMatch: {b: {$eq: 5}}}}"));
    unique_ptr<CanonicalQuery> objElemMatchString(
        canonicalize("{a: {$elemMatch: {b: {$eq: 'str'}}}}"));
    assertPlanCacheKeysUnequalDueToDiscriminators(makeKey(*objElemMatchNoString, indexCores),
                                                  makeKey(*objElemMatchString, indexCores));

    // Object elemMatch with multiple predicates on "a.b" should detect string comparison when query
    // collation differs.
    unique_ptr<CanonicalQuery> objElemMatchWithOrNoString(
        canonicalize("{a: {$elemMatch: {$or: [{b: {$eq: 5}}, {b: {$eq: 10}}]}}}"));
    unique_ptr<CanonicalQuery> objElemMatchWithOrString(
        canonicalize("{a: {$elemMatch: {$or: [{b: {$eq: 'A'}}, {b: {$eq: 10}}]}}}"));

    assertPlanCacheKeysUnequalDueToDiscriminators(makeKey(*objElemMatchWithOrNoString, indexCores),
                                                  makeKey(*objElemMatchWithOrString, indexCores));
}

TEST_F(PlanCacheKeyInfoTest, ComputeKeyWithNot) {
    const auto keyPattern = BSON("a.b" << 1);
    const auto keyPattern2 = BSON("a" << 1);

    const std::vector<CoreIndexInfo> indexCores = {
        CoreIndexInfo(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                      false,                       // sparse
                      IndexEntry::Identifier{""},  // name
                      nullptr,                     // filterExpr
                      nullptr),                    // collation
        CoreIndexInfo(keyPattern2,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern2)),
                      false,                       // sparse
                      IndexEntry::Identifier{""},  // name
                      nullptr,                     // filterExpr
                      nullptr),                    // collation
    };

    // $not with a child that does array matching should be differentiated from $not when the child
    // does not do array matching.
    unique_ptr<CanonicalQuery> dottedMatchNoArray(canonicalize("{'a.b': {$not: {$eq: 5}}}"));
    unique_ptr<CanonicalQuery> dottedMatchArray(canonicalize("{'a.b': {$not: {$eq: []}}}"));
    assertPlanCacheKeysUnequalDueToDiscriminators(makeKey(*dottedMatchNoArray, indexCores),
                                                  makeKey(*dottedMatchArray, indexCores));

    unique_ptr<CanonicalQuery> matchNoArray(canonicalize("{a: {$not: {$eq: 5}}}"));
    unique_ptr<CanonicalQuery> matchArray(canonicalize("{a: {$not: {$eq: []}}}"));
    assertPlanCacheKeysUnequalDueToDiscriminators(makeKey(*matchNoArray, indexCores),
                                                  makeKey(*matchArray, indexCores));


    // Tests the same as above, but where the $not is under $elemMatch.
    unique_ptr<CanonicalQuery> objElemMatchNoArray(
        canonicalize("{a: {$elemMatch: {b: {$not: {$eq: 5}}}}}"));
    unique_ptr<CanonicalQuery> objElemMatchArray(
        canonicalize("{a: {$elemMatch: {b: {$not: {$eq: []}}}}}"));
    assertPlanCacheKeysUnequalDueToDiscriminators(makeKey(*objElemMatchNoArray, indexCores),
                                                  makeKey(*objElemMatchArray, indexCores));

    unique_ptr<CanonicalQuery> valueElemMatchNoArray(
        canonicalize("{a: {$elemMatch: {$not: {$eq: 5}}}}"));
    unique_ptr<CanonicalQuery> valueElemMatchArray(
        canonicalize("{a: {$elemMatch: {$not: {$eq: []}}}}"));
    assertPlanCacheKeysUnequalDueToDiscriminators(makeKey(*valueElemMatchNoArray, indexCores),
                                                  makeKey(*valueElemMatchArray, indexCores));
}

TEST_F(PlanCacheKeyInfoTest, ComputeKeyWithLogicalNodes) {
    // Test that we don't include discriminators for logical nodes $and/$or, since those have no
    // effect on indexability.
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    const auto keyPattern = BSON("a" << 1);
    const auto dottedKeyPattern = BSON("a.b" << 1);
    const std::vector<CoreIndexInfo> indexCores = {
        CoreIndexInfo(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                      false,                       // sparse
                      IndexEntry::Identifier{""},  // name
                      nullptr,                     // filterExpr
                      &collator),                  // collation
        CoreIndexInfo(dottedKeyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(dottedKeyPattern)),
                      false,                       // sparse
                      IndexEntry::Identifier{""},  // name
                      nullptr,                     // filterExpr
                      &collator)};                 // collation

    // Top-level logical nodes have no path, so they shouldn't be included.
    unique_ptr<CanonicalQuery> simpleAnd(canonicalize("{$and: [{a: 1}, {b: 1}]}"));
    ASSERT_EQ(makeKey(*simpleAnd, indexCores).getIndexabilityDiscriminators(), "<1>");
    unique_ptr<CanonicalQuery> simpleOr(canonicalize("{$or: [{a: 1}, {b: 1}]}"));
    ASSERT_EQ(makeKey(*simpleOr, indexCores).getIndexabilityDiscriminators(), "<1>");
    unique_ptr<CanonicalQuery> orAnd(canonicalize("{$or: [{a: 1, b: 1}, {a: 5, b: 5}]}"));
    ASSERT_EQ(makeKey(*orAnd, indexCores).getIndexabilityDiscriminators(), "<1><1>");
    unique_ptr<CanonicalQuery> andOr(canonicalize("{a: 1, $or: [{c: 1}, {b: 1}]}"));
    ASSERT_EQ(makeKey(*andOr, indexCores).getIndexabilityDiscriminators(), "<1>");

    // Similar tests, but under an $elemMatch. Expect entries for the $elemMatch on "a" and each
    // nested comparison on "a.b", and no other entries.
    unique_ptr<CanonicalQuery> elemMatchSimpleAnd(
        canonicalize("{a: {$elemMatch: {$and: [{c: 1}, {b: 1}]}}}"));
    ASSERT_EQ(makeKey(*elemMatchSimpleAnd, indexCores).getIndexabilityDiscriminators(), "<1><1>");
    unique_ptr<CanonicalQuery> elemMatchSimpleOr(
        canonicalize("{a: {$elemMatch: {$or: [{c: 1}, {b: 1}]}}}"));
    ASSERT_EQ(makeKey(*elemMatchSimpleOr, indexCores).getIndexabilityDiscriminators(), "<1><1>");
    unique_ptr<CanonicalQuery> elemMatchWithLogicalNodes(
        canonicalize("{a: {$elemMatch: {$or: [{$and: [{b: {$gte: 3}}, {b: {$lte: 5}}]}, {$and: "
                     "[{b: {$gte: 1}}, {b: {$lte: 2}}]}]}}}"));
    ASSERT_EQ(makeKey(*elemMatchWithLogicalNodes, indexCores).getIndexabilityDiscriminators(),
              "<1><1><1><1><1>");
}

TEST_F(PlanCacheKeyInfoTest, ComputeKeyWildcardIndex) {
    auto entryProjUpdatePair = makeWildcardUpdate(BSON("a.$**" << 1));

    const std::vector<CoreIndexInfo> indexCores = {entryProjUpdatePair.first};

    // Used to check that two queries have the same shape when no indexes are present.
    PlanCache planCacheWithNoIndexes(5000);

    // Compatible with index.
    unique_ptr<CanonicalQuery> usesPathWithScalar(canonicalize("{a: 'abcdef'}"));
    unique_ptr<CanonicalQuery> usesPathWithEmptyArray(canonicalize("{a: []}"));

    // Not compatible with index.
    unique_ptr<CanonicalQuery> usesPathWithObject(canonicalize("{a: {b: 'abc'}}"));
    unique_ptr<CanonicalQuery> usesPathWithArray(canonicalize("{a: [1, 2]}"));
    unique_ptr<CanonicalQuery> usesPathWithArrayContainingObject(canonicalize("{a: [1, {b: 1}]}"));
    unique_ptr<CanonicalQuery> usesPathWithEmptyObject(canonicalize("{a: {}}"));
    unique_ptr<CanonicalQuery> doesNotUsePath(canonicalize("{b: 1234}"));

    // Check that the queries which are compatible with the index have the same key.
    ASSERT_EQ(makeKey(*usesPathWithScalar, indexCores),
              makeKey(*usesPathWithEmptyArray, indexCores));

    // Check that the queries which have the same path as the index, but aren't supported, have
    // different keys.
    ASSERT_EQ(makeKey(*usesPathWithScalar), makeKey(*usesPathWithObject));
    assertPlanCacheKeysUnequalDueToDiscriminators(makeKey(*usesPathWithScalar, indexCores),
                                                  makeKey(*usesPathWithObject, indexCores));
    ASSERT_EQ(makeKey(*usesPathWithScalar, indexCores).getIndexabilityDiscriminators(), "<1>");
    ASSERT_EQ(makeKey(*usesPathWithObject, indexCores).getIndexabilityDiscriminators(), "<0>");

    ASSERT_EQ(makeKey(*usesPathWithObject, indexCores), makeKey(*usesPathWithArray, indexCores));
    ASSERT_EQ(makeKey(*usesPathWithObject, indexCores),
              makeKey(*usesPathWithArrayContainingObject, indexCores));

    // The query on 'b' should have a completely different plan cache key (both with and without a
    // wildcard index).
    ASSERT_NE(makeKey(*usesPathWithScalar), makeKey(*doesNotUsePath));
    ASSERT_NE(makeKey(*usesPathWithScalar, indexCores), makeKey(*doesNotUsePath, indexCores));
    ASSERT_NE(makeKey(*usesPathWithObject), makeKey(*doesNotUsePath));
    ASSERT_NE(makeKey(*usesPathWithObject, indexCores), makeKey(*doesNotUsePath, indexCores));

    // More complex queries with similar shapes. This is to ensure that plan cache key encoding
    // correctly traverses the expression tree.
    auto orQueryWithOneBranchAllowed = canonicalize("{$or: [{a: 3}, {a: {$gt: [1,2]}}]}");
    // Same shape except 'a' is compared to an object.
    auto orQueryWithNoBranchesAllowed =
        canonicalize("{$or: [{a: {someobject: 1}}, {a: {$gt: [1,2]}}]}");
    // The two queries should have the same shape when no indexes are present, but different shapes
    // when a $** index is present.
    ASSERT_EQ(makeKey(*orQueryWithOneBranchAllowed), makeKey(*orQueryWithNoBranchesAllowed));
    assertPlanCacheKeysUnequalDueToDiscriminators(
        makeKey(*orQueryWithOneBranchAllowed, indexCores),
        makeKey(*orQueryWithNoBranchesAllowed, indexCores));
    ASSERT_EQ(makeKey(*orQueryWithOneBranchAllowed, indexCores).getIndexabilityDiscriminators(),
              "<1><0>");
    ASSERT_EQ(makeKey(*orQueryWithNoBranchesAllowed, indexCores).getIndexabilityDiscriminators(),
              "<0><0>");
}

TEST_F(PlanCacheKeyInfoTest, ComputeKeyWildcardIndexDiscriminatesEqualityToEmptyObj) {
    auto entryProjUpdatePair = makeWildcardUpdate(BSON("a.$**" << 1));

    const std::vector<CoreIndexInfo> indexCores = {entryProjUpdatePair.first};

    // Equality to empty obj and equality to non-empty obj have different plan cache keys.
    std::unique_ptr<CanonicalQuery> equalsEmptyObj(canonicalize("{a: {}}"));
    std::unique_ptr<CanonicalQuery> equalsNonEmptyObj(canonicalize("{a: {b: 1}}"));
    assertPlanCacheKeysUnequalDueToDiscriminators(makeKey(*equalsEmptyObj, indexCores),
                                                  makeKey(*equalsNonEmptyObj, indexCores));
    ASSERT_EQ(makeKey(*equalsNonEmptyObj, indexCores).getIndexabilityDiscriminators(), "<0>");
    ASSERT_EQ(makeKey(*equalsEmptyObj, indexCores).getIndexabilityDiscriminators(), "<1>");

    // $in with empty obj and $in with non-empty obj have different plan cache keys.
    std::unique_ptr<CanonicalQuery> inWithEmptyObj(canonicalize("{a: {$in: [{}]}}"));
    std::unique_ptr<CanonicalQuery> inWithNonEmptyObj(canonicalize("{a: {$in: [{b: 1}]}}"));
    assertPlanCacheKeysUnequalDueToDiscriminators(makeKey(*inWithEmptyObj, indexCores),
                                                  makeKey(*inWithNonEmptyObj, indexCores));
    ASSERT_EQ(makeKey(*inWithNonEmptyObj, indexCores).getIndexabilityDiscriminators(), "<0>");
    ASSERT_EQ(makeKey(*inWithEmptyObj, indexCores).getIndexabilityDiscriminators(), "<1>");
}

TEST_F(PlanCacheKeyInfoTest,
       ComputeKeyWildcardDiscriminatesCorrectlyBasedOnPartialFilterExpression) {
    BSONObj filterObj = BSON("x" << BSON("$gt" << 0));
    std::unique_ptr<MatchExpression> filterExpr(parseMatchExpression(filterObj));

    auto entryProjUpdatePair = makeWildcardUpdate(BSON("$**" << 1));
    auto indexInfo = std::move(entryProjUpdatePair.first);
    indexInfo.filterExpr = filterExpr.get();

    const std::vector<CoreIndexInfo> indexCores = {indexInfo};

    // Test that queries on field 'x' are discriminated based on their relationship with the partial
    // filter expression.
    {
        auto compatibleWithFilter = canonicalize("{x: {$eq: 5}}");
        auto incompatibleWithFilter = canonicalize("{x: {$eq: -5}}");
        auto compatibleKey = makeKey(*compatibleWithFilter, indexCores);
        auto incompatibleKey = makeKey(*incompatibleWithFilter, indexCores);

        assertPlanCacheKeysUnequalDueToDiscriminators(compatibleKey, incompatibleKey);
        // The discriminator strings have the format "<xx>". That is, there are two discriminator
        // bits for the "x" predicate, the first pertaining to the partialFilterExpression and the
        // second around applicability to the wildcard index.
        ASSERT_EQ(compatibleKey.getIndexabilityDiscriminators(), "(1)<1>");
        ASSERT_EQ(incompatibleKey.getIndexabilityDiscriminators(), "(0)<1>");
    }

    // The partialFilterExpression should lead to a discriminator over field 'x', but not over 'y'.
    // (Separately, there are wildcard-related discriminator bits for both 'x' and 'y'.)
    {
        auto compatibleWithFilter = canonicalize("{x: {$eq: 5}, y: 1}");
        auto incompatibleWithFilter = canonicalize("{x: {$eq: -5}, y: 1}");
        auto compatibleKey = makeKey(*compatibleWithFilter, indexCores);
        auto incompatibleKey = makeKey(*incompatibleWithFilter, indexCores);

        assertPlanCacheKeysUnequalDueToDiscriminators(compatibleKey, incompatibleKey);
        // The discriminator strings have the format "<xx><y>". That is, there are two discriminator
        // bits for the "x" predicate (the first pertaining to the partialFilterExpression, the
        // second around applicability to the wildcard index) and one discriminator bit for "y".
        ASSERT_EQ(compatibleKey.getIndexabilityDiscriminators(), "(1)<1><1>");
        ASSERT_EQ(incompatibleKey.getIndexabilityDiscriminators(), "(000)<1><1>");
    }

    // $eq:null predicates cannot be assigned to a wildcard index. Make sure that this is
    // discrimated correctly. This test is designed to reproduce SERVER-48614.
    {
        auto compatibleQuery = canonicalize("{x: {$eq: 5}, y: 1}");
        auto incompatibleQuery = canonicalize("{x: {$eq: 5}, y: null}");
        auto compatibleKey = makeKey(*compatibleQuery, indexCores);
        auto incompatibleKey = makeKey(*incompatibleQuery, indexCores);

        assertPlanCacheKeysUnequalDueToDiscriminators(compatibleKey, incompatibleKey);
        // The discriminator strings have the format "<xx><y>". That is, there are two discriminator
        // bits for the "x" predicate (the first pertaining to the partialFilterExpression, the
        // second around applicability to the wildcard index) and one discriminator bit for "y".
        ASSERT_EQ(compatibleKey.getIndexabilityDiscriminators(), "(1)<1><1>");
        ASSERT_EQ(incompatibleKey.getIndexabilityDiscriminators(), "(1)<1><0>");
    }

    // Test that the discriminators are correct for an $eq:null predicate on 'x'. This predicate is
    // imcompatible for two reasons: null equality predicates cannot be answered by wildcard
    // indexes, and the predicate is not compatible with the partial filter expression. This should
    // result in two "0" bits inside the discriminator string.
    {
        auto key = makeKey(*canonicalize("{x: {$eq: null}}"), indexCores);
        ASSERT_EQ(key.getIndexabilityDiscriminators(), "(0)<0>");
    }
}

TEST_F(PlanCacheKeyInfoTest,
       ComputeKeyWildcardDiscriminatesCorrectlyWithPartialFilterAndExpression) {
    // Partial filter is an AND of multiple conditions.
    BSONObj filterObj = BSON("x" << BSON("$gt" << 0) << "y" << BSON("$gt" << 0));
    std::unique_ptr<MatchExpression> filterExpr(parseMatchExpression(filterObj));

    auto entryProjUpdatePair = makeWildcardUpdate(BSON("$**" << 1));
    auto indexInfo = std::move(entryProjUpdatePair.first);
    indexInfo.filterExpr = filterExpr.get();

    const std::vector<CoreIndexInfo> indexCores = {indexInfo};

    {
        // TODO update The discriminators should have the format <xx><yy><z>. The 'z' predicate has
        // just one discriminator because it is not referenced in the partial filter expression. All
        // predicates are compatible.
        auto key = makeKey(*canonicalize("{x: {$eq: 1}, y: {$eq: 2}, z: {$eq: 3}}"), indexCores);
        ASSERT_EQ(key.getIndexabilityDiscriminators(), "(1)<1><1><1>");
    }

    {
        // The discriminators should have the format <xx><yy><z>. The 'y' predicate is not
        // compatible with the partial filter expression, leading to one of the 'y' bits being set
        // to zero.
        auto key = makeKey(*canonicalize("{x: {$eq: 1}, y: {$eq: -2}, z: {$eq: 3}}"), indexCores);
        ASSERT_EQ(key.getIndexabilityDiscriminators(), "(0000)<1><1><1>");
    }
}

TEST_F(PlanCacheKeyInfoTest,
       ComputeKeyDiscriminatesCorrectlyWithPartialFilterAndWildcardProjection) {
    BSONObj filterObj = BSON("x" << BSON("$gt" << 0));
    std::unique_ptr<MatchExpression> filterExpr(parseMatchExpression(filterObj));

    auto entryProjUpdatePair = makeWildcardUpdate(BSON("y.$**" << 1));
    auto indexInfo = std::move(entryProjUpdatePair.first);
    indexInfo.filterExpr = filterExpr.get();

    const std::vector<CoreIndexInfo> indexCores = {indexInfo};

    {
        // The discriminators have the format <x><y>. The discriminator for 'x' indicates whether
        // the predicate is compatible with the partial filter expression, whereas the disciminator
        // for 'y' is about compatibility with the wildcard index.
        auto key = makeKey(*canonicalize("{x: {$eq: 1}, y: {$eq: 2}, z: {$eq: 3}}"), indexCores);
        ASSERT_EQ(key.getIndexabilityDiscriminators(), "(1)<1>");
    }

    {
        // Similar to the previous case, except with an 'x' predicate that is incompatible with the
        // partial filter expression.
        auto key = makeKey(*canonicalize("{x: {$eq: -1}, y: {$eq: 2}, z: {$eq: 3}}"), indexCores);
        ASSERT_EQ(key.getIndexabilityDiscriminators(), "(0000)<1>");
    }

    {
        // Case where the 'y' predicate is not compatible with the wildcard index.
        auto key = makeKey(*canonicalize("{x: {$eq: 1}, y: {$eq: null}, z: {$eq: 3}}"), indexCores);
        ASSERT_EQ(key.getIndexabilityDiscriminators(), "(1)<0>");
    }
}

TEST_F(PlanCacheKeyInfoTest,
       ComputeKeyWildcardDiscriminatesCorrectlyWithPartialFilterOnNestedField) {
    BSONObj filterObj = BSON("x.y" << BSON("$gt" << 0));
    std::unique_ptr<MatchExpression> filterExpr(parseMatchExpression(filterObj));

    auto entryProjUpdatePair = makeWildcardUpdate(BSON("$**" << 1));
    auto indexInfo = std::move(entryProjUpdatePair.first);
    indexInfo.filterExpr = filterExpr.get();

    const std::vector<CoreIndexInfo> indexCores = {indexInfo};

    {
        // The discriminators have the format <x><(x.y)(x.y)<y>. All predicates are compatible
        auto key =
            makeKey(*canonicalize("{x: {$eq: 1}, y: {$eq: 2}, 'x.y': {$eq: 3}}"), indexCores);
        ASSERT_EQ(key.getIndexabilityDiscriminators(), "(1)<1><1><1>");
    }

    {
        // Here, the predicate on "x.y" is not compatible with the partial filter expression.
        auto key =
            makeKey(*canonicalize("{x: {$eq: 1}, y: {$eq: 2}, 'x.y': {$eq: -3}}"), indexCores);
        ASSERT_EQ(key.getIndexabilityDiscriminators(), "(0000)<1><1><1>");
    }
}

TEST_F(PlanCacheKeyInfoTest, StableKeyDoesNotChangeAcrossIndexCreation) {
    unique_ptr<CanonicalQuery> cq(canonicalize("{a: 0}"));
    const auto preIndexKey = makeKey(*cq);
    const auto preIndexStableKey = preIndexKey.getQueryShape();
    ASSERT_EQ(preIndexKey.getIndexabilityDiscriminators(), "");

    const auto keyPattern = BSON("a" << 1);
    // Create a sparse index (which requires a discriminator).
    const std::vector<CoreIndexInfo> indexCores = {
        CoreIndexInfo(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                      true,                          // sparse
                      IndexEntry::Identifier{""})};  // name

    const auto postIndexKey = makeKey(*cq, indexCores);
    const auto postIndexStableKey = postIndexKey.getQueryShape();
    ASSERT_NE(preIndexKey, postIndexKey);
    ASSERT_EQ(preIndexStableKey, postIndexStableKey);
    ASSERT_EQ(postIndexKey.getIndexabilityDiscriminators(), "<1>");
}

TEST_F(PlanCacheKeyInfoTest, ComputeKeyNotEqualsArray) {
    unique_ptr<CanonicalQuery> cqNeArray(canonicalize("{a: {$ne: [1]}}"));
    unique_ptr<CanonicalQuery> cqNeScalar(canonicalize("{a: {$ne: 123}}"));

    const auto noIndexNeArrayKey = makeKey(*cqNeArray);
    const auto noIndexNeScalarKey = makeKey(*cqNeScalar);
    ASSERT_EQ(noIndexNeArrayKey.getIndexabilityDiscriminators(), "<0>");
    ASSERT_EQ(noIndexNeScalarKey.getIndexabilityDiscriminators(), "<1>");
    ASSERT_EQ(noIndexNeScalarKey.getQueryShape(), noIndexNeArrayKey.getQueryShape());

    const auto keyPattern = BSON("a" << 1);
    // Create a normal btree index. It will have a discriminator.
    const std::vector<CoreIndexInfo> indexCores = {
        CoreIndexInfo(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                      false,                         // sparse
                      IndexEntry::Identifier{""})};  // name*/

    const auto withIndexNeArrayKey = makeKey(*cqNeArray, indexCores);
    const auto withIndexNeScalarKey = makeKey(*cqNeScalar, indexCores);

    ASSERT_NE(noIndexNeArrayKey, withIndexNeArrayKey);
    ASSERT_EQ(noIndexNeArrayKey.getQueryShape(), withIndexNeArrayKey.getQueryShape());

    ASSERT_EQ(noIndexNeScalarKey.getQueryShape(), withIndexNeScalarKey.getQueryShape());
    // There will be one discriminator for the $not and another for the leaf node ({$eq: 123}).
    ASSERT_EQ(withIndexNeScalarKey.getIndexabilityDiscriminators(), "<1><1>");
    // There will be one discriminator for the $not and another for the leaf node ({$eq: [1]}).
    // Since the index can support equality to an array, the second discriminator will have a value
    // of '1'.
    ASSERT_EQ(withIndexNeArrayKey.getIndexabilityDiscriminators(), "<0><1>");
}

TEST_F(PlanCacheKeyInfoTest, ComputeKeyNinArray) {
    unique_ptr<CanonicalQuery> cqNinArray(canonicalize("{a: {$nin: [123, [1]]}}"));
    unique_ptr<CanonicalQuery> cqNinScalar(canonicalize("{a: {$nin: [123, 456]}}"));

    const auto noIndexNinArrayKey = makeKey(*cqNinArray);
    const auto noIndexNinScalarKey = makeKey(*cqNinScalar);
    ASSERT_EQ(noIndexNinArrayKey.getIndexabilityDiscriminators(), "<0>");
    ASSERT_EQ(noIndexNinScalarKey.getIndexabilityDiscriminators(), "<1>");
    ASSERT_EQ(noIndexNinScalarKey.getQueryShape(), noIndexNinArrayKey.getQueryShape());

    const auto keyPattern = BSON("a" << 1);
    // Create a normal btree index. It will have a discriminator.
    const std::vector<CoreIndexInfo> indexCores = {
        CoreIndexInfo(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                      false,                         // sparse
                      IndexEntry::Identifier{""})};  // name

    const auto withIndexNinArrayKey = makeKey(*cqNinArray, indexCores);
    const auto withIndexNinScalarKey = makeKey(*cqNinScalar, indexCores);

    // The unstable part of the key for $nin: [<array>] should have changed. The stable part,
    // however, should not.
    ASSERT_EQ(noIndexNinArrayKey.getQueryShape(), withIndexNinArrayKey.getQueryShape());
    ASSERT_NE(noIndexNinArrayKey.getIndexabilityDiscriminators(),
              withIndexNinArrayKey.getIndexabilityDiscriminators());

    ASSERT_EQ(noIndexNinScalarKey.getQueryShape(), withIndexNinScalarKey.getQueryShape());
    ASSERT_EQ(withIndexNinArrayKey.getIndexabilityDiscriminators(), "<0><1>");
    ASSERT_EQ(withIndexNinScalarKey.getIndexabilityDiscriminators(), "<1><1>");
}

// Test for a bug which would be easy to introduce. If we only inserted discriminators for some
// nodes, we would have a problem. For example if our "stable" key was:
// (or[nt[eqa],nt[eqa]])
// And there was just one discriminator:
// <0>

// Whether the discriminator referred to the first not-eq node or the second would be
// ambiguous. This would make it possible for two queries with different shapes (and different
// plans) to get the same plan cache key. We test that this does not happen for a simple example.
TEST_F(PlanCacheKeyInfoTest, PlanCacheKeyCollision) {
    unique_ptr<CanonicalQuery> cqNeA(canonicalize("{$or: [{a: {$ne: 5}}, {a: {$ne: [12]}}]}"));
    unique_ptr<CanonicalQuery> cqNeB(canonicalize("{$or: [{a: {$ne: [12]}}, {a: {$ne: 5}}]}"));

    const auto keyA = makeKey(*cqNeA);
    const auto keyB = makeKey(*cqNeB);
    ASSERT_EQ(keyA.getQueryShape(), keyB.getQueryShape());
    ASSERT_NE(keyA.getIndexabilityDiscriminators(), keyB.getIndexabilityDiscriminators());
    const auto keyPattern = BSON("a" << 1);
    // Create a normal btree index. It will have a discriminator.
    std::vector<CoreIndexInfo> indexCores = {
        CoreIndexInfo(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                      false,                         // sparse
                      IndexEntry::Identifier{""})};  // name
    const auto keyAWithIndex = makeKey(*cqNeA, indexCores);
    const auto keyBWithIndex = makeKey(*cqNeB, indexCores);

    ASSERT_EQ(keyAWithIndex.getQueryShape(), keyBWithIndex.getQueryShape());
    ASSERT_NE(keyAWithIndex.getIndexabilityDiscriminators(),
              keyBWithIndex.getIndexabilityDiscriminators());
}
}  // namespace mongo
