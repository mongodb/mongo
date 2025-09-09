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

#include "mongo/db/query/canonical_distinct.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/parsed_distinct_command.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

static const NamespaceString testns =
    NamespaceString::createNamespaceString_forTest("testdb.testcoll");
static const bool isExplain = true;

class CanonicalDistinctTest : public mongo::unittest::Test {
public:
    CanonicalDistinctTest() {
        uniqueTxn = serviceContext.makeOperationContext();
        opCtx = uniqueTxn.get();
        expCtx = ExpressionContextBuilder{}
                     .opCtx(opCtx)
                     .ns(testns)
                     .tmpDir(boost::filesystem::path(storageGlobalParams.dbpath) / "_tmp")
                     .build();
    }

private:
    QueryTestServiceContext serviceContext;
    ServiceContext::UniqueOperationContext uniqueTxn;
    OperationContext* opCtx;

protected:
    boost::intrusive_ptr<ExpressionContext> expCtx;
};

std::unique_ptr<ParsedDistinctCommand> bsonToParsedDistinct(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const BSONObj& cmd) {
    auto distinctCommand = std::make_unique<DistinctCommandRequest>(DistinctCommandRequest::parse(
        cmd,
        IDLParserContext("distinctCommandRequest",
                         auth::ValidatedTenancyScope::get(expCtx->getOperationContext()),
                         boost::none,
                         SerializationContext::stateDefault())));
    return parsed_distinct_command::parse(expCtx,
                                          std::move(distinctCommand),
                                          ExtensionsCallbackNoop(),
                                          MatchExpressionParser::kAllowAllSpecialFeatures);
}

TEST_F(CanonicalDistinctTest, ConvertToAggregationNoQuery) {
    auto rawCmd = fromjson("{distinct: 'testcoll', key: 'x', $db: 'testdb'}");
    auto pdc = bsonToParsedDistinct(expCtx, rawCmd);
    auto canonicalQuery =
        parsed_distinct_command::parseCanonicalQuery(expCtx, std::move(pdc), nullptr);

    auto agg = parsed_distinct_command::asAggregation(
        *canonicalQuery, boost::none, SerializationContext::stateCommandRequest());

    ASSERT(!agg.getExplain());
    ASSERT_EQ(
        agg.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
        aggregation_request_helper::kDefaultBatchSize);
    ASSERT_EQ(agg.getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(agg.getCollation().value_or(BSONObj()), BSONObj());
    ASSERT(!agg.getReadConcern().has_value());
    ASSERT(agg.getUnwrappedReadPref().value_or(BSONObj()).isEmpty());
    ASSERT_EQUALS(agg.getMaxTimeMS().value_or(0), 0u);

    std::vector<BSONObj> expectedPipeline{
        BSON("$replaceRoot" << BSON("newRoot"
                                    << BSON("_internalUnwoundArray"
                                            << BSON("$_internalFindAllValuesAtPath" << "x")))),
        BSON("$unwind" << BSON("path" << "$_internalUnwoundArray"
                                      << "preserveNullAndEmptyArrays" << true)),
        BSON("$group" << BSON("_id" << BSONNULL << "distinct"
                                    << BSON("$addToSet" << "$_internalUnwoundArray")))};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      agg.getPipeline().begin(),
                      agg.getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST_F(CanonicalDistinctTest, ConvertToAggregationDottedPathNoQuery) {
    auto rawCmd = fromjson("{distinct: 'testcoll', key: 'x.y.z', $db: 'testdb'}");
    auto pdc = bsonToParsedDistinct(expCtx, rawCmd);
    auto canonicalQuery =
        parsed_distinct_command::parseCanonicalQuery(expCtx, std::move(pdc), nullptr);

    auto agg = parsed_distinct_command::asAggregation(
        *canonicalQuery, boost::none, SerializationContext::stateCommandRequest());

    ASSERT(!agg.getExplain());
    ASSERT_EQ(
        agg.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
        aggregation_request_helper::kDefaultBatchSize);
    ASSERT_EQ(agg.getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(agg.getCollation().value_or(BSONObj()), BSONObj());
    ASSERT(!agg.getReadConcern().has_value());
    ASSERT(agg.getUnwrappedReadPref().value_or(BSONObj()).isEmpty());
    ASSERT_EQUALS(agg.getMaxTimeMS().value_or(0), 0u);

    std::vector<BSONObj> expectedPipeline{
        BSON("$replaceRoot" << BSON("newRoot"
                                    << BSON("_internalUnwoundArray"
                                            << BSON("$_internalFindAllValuesAtPath" << "x.y.z")))),
        BSON("$unwind" << BSON("path" << "$_internalUnwoundArray"
                                      << "preserveNullAndEmptyArrays" << true)),
        BSON("$group" << BSON("_id" << BSONNULL << "distinct"
                                    << BSON("$addToSet" << "$_internalUnwoundArray")))};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      agg.getPipeline().begin(),
                      agg.getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST_F(CanonicalDistinctTest, ConvertToAggregationWithAllOptions) {
    auto rawCmd =
        BSON("distinct" << "testcoll"
                        << "key"
                        << "x"
                        << "hint" << BSON("b" << 5) << "collation" << BSON("locale" << "en_US")
                        << "readConcern" << BSON("level" << "linearizable") << "$queryOptions"
                        << BSON("readPreference" << "secondary") << "maxTimeMS" << 100 << "$db"
                        << "testdb");
    auto pdc = bsonToParsedDistinct(expCtx, rawCmd);
    auto canonicalQuery =
        parsed_distinct_command::parseCanonicalQuery(expCtx, std::move(pdc), nullptr);
    auto agg = parsed_distinct_command::asAggregation(
        *canonicalQuery, boost::none, SerializationContext::stateCommandRequest());

    ASSERT(!agg.getExplain());
    ASSERT_EQ(
        agg.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
        aggregation_request_helper::kDefaultBatchSize);
    ASSERT_EQ(agg.getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(agg.getCollation().value_or(BSONObj()), BSON("locale" << "en_US"));
    ASSERT_BSONOBJ_EQ(agg.getReadConcern().value_or(repl::ReadConcernArgs()).toBSONInner(),
                      BSON("level" << "linearizable"));
    ASSERT_BSONOBJ_EQ(agg.getUnwrappedReadPref().value_or(BSONObj()),
                      BSON("readPreference" << "secondary"));
    ASSERT_EQUALS(agg.getMaxTimeMS().value_or(0), 100u);
    ASSERT_BSONOBJ_EQ(agg.getHint().value_or(BSONObj()), fromjson("{ b : 5 }"));

    std::vector<BSONObj> expectedPipeline{
        BSON("$replaceRoot" << BSON("newRoot"
                                    << BSON("_internalUnwoundArray"
                                            << BSON("$_internalFindAllValuesAtPath" << "x")))),
        BSON("$unwind" << BSON("path" << "$_internalUnwoundArray"
                                      << "preserveNullAndEmptyArrays" << true)),
        BSON("$group" << BSON("_id" << BSONNULL << "distinct"
                                    << BSON("$addToSet" << "$_internalUnwoundArray")))};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      agg.getPipeline().begin(),
                      agg.getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST_F(CanonicalDistinctTest, ConvertToAggregationWithQuery) {
    auto rawCmd = fromjson("{distinct: 'testcoll', key: 'y', query: {z: 7}, $db: 'testdb'}");
    auto pdc = bsonToParsedDistinct(expCtx, rawCmd);
    auto canonicalQuery =
        parsed_distinct_command::parseCanonicalQuery(expCtx, std::move(pdc), nullptr);

    auto agg = parsed_distinct_command::asAggregation(
        *canonicalQuery, boost::none, SerializationContext::stateCommandRequest());

    ASSERT(!agg.getExplain());
    ASSERT_EQ(
        agg.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
        aggregation_request_helper::kDefaultBatchSize);
    ASSERT_EQ(agg.getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(agg.getCollation().value_or(BSONObj()), BSONObj());
    ASSERT(!agg.getReadConcern().has_value());
    ASSERT(agg.getUnwrappedReadPref().value_or(BSONObj()).isEmpty());
    ASSERT_EQUALS(agg.getMaxTimeMS().value_or(0), 0u);

    std::vector<BSONObj> expectedPipeline{
        BSON("$match" << BSON("z" << 7)),
        BSON("$replaceRoot" << BSON("newRoot"
                                    << BSON("_internalUnwoundArray"
                                            << BSON("$_internalFindAllValuesAtPath" << "y")))),
        BSON("$unwind" << BSON("path" << "$_internalUnwoundArray"
                                      << "preserveNullAndEmptyArrays" << true)),
        BSON("$group" << BSON("_id" << BSONNULL << "distinct"
                                    << BSON("$addToSet" << "$_internalUnwoundArray")))};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      agg.getPipeline().begin(),
                      agg.getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST_F(CanonicalDistinctTest, ExplainNotIncludedWhenConvertingToAggregationCommand) {
    auto rawCmd = fromjson("{distinct: 'testcoll', key: 'x', $db: 'testdb'}");
    auto pdc = bsonToParsedDistinct(expCtx, rawCmd);
    auto canonicalQuery =
        parsed_distinct_command::parseCanonicalQuery(expCtx, std::move(pdc), nullptr);

    auto agg = parsed_distinct_command::asAggregation(
        *canonicalQuery, boost::none, SerializationContext::stateCommandRequest());

    ASSERT(!agg.getExplain());
    ASSERT_EQ(agg.getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(agg.getCollation().value_or(BSONObj()), BSONObj());

    std::vector<BSONObj> expectedPipeline{
        BSON("$replaceRoot" << BSON("newRoot"
                                    << BSON("_internalUnwoundArray"
                                            << BSON("$_internalFindAllValuesAtPath" << "x")))),
        BSON("$unwind" << BSON("path" << "$_internalUnwoundArray"
                                      << "preserveNullAndEmptyArrays" << true)),
        BSON("$group" << BSON("_id" << BSONNULL << "distinct"
                                    << BSON("$addToSet" << "$_internalUnwoundArray")))};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      agg.getPipeline().begin(),
                      agg.getPipeline().end(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST_F(CanonicalDistinctTest, CopyMaintainsDistinctProperties) {
    auto rawCmd = fromjson("{distinct: 'testcoll', key: 'x', $db: 'testdb'}");
    auto pdc = bsonToParsedDistinct(expCtx, rawCmd);
    auto canonicalQuery =
        parsed_distinct_command::parseCanonicalQuery(expCtx, std::move(pdc), nullptr);
    const auto& originalDistinct = canonicalQuery->getDistinct();
    originalDistinct->setProjectionSpec(BSON("_id" << 0 << "x" << 1));
    originalDistinct->setSortRequirement(SortPattern(BSON("x" << 1), expCtx));

    const auto& copyDistinct = CanonicalDistinct(*originalDistinct);
    ASSERT_NE(&originalDistinct, &copyDistinct);

    ASSERT_EQ(originalDistinct->getKey(), copyDistinct.getKey());
    ASSERT_EQ(originalDistinct->isMirrored(), copyDistinct.isMirrored());
    ASSERT_EQ(originalDistinct->getSampleId(), copyDistinct.getSampleId());
    ASSERT_BSONOBJ_EQ(originalDistinct->getProjectionSpec().get(),
                      copyDistinct.getProjectionSpec().get());
    ASSERT_EQ(originalDistinct->isDistinctScanDirectionFlipped(),
              copyDistinct.isDistinctScanDirectionFlipped());
    ASSERT_EQ(originalDistinct->getSortRequirement(), copyDistinct.getSortRequirement());
    ASSERT_BSONOBJ_EQ(originalDistinct->getSerializedSortRequirement(),
                      copyDistinct.getSerializedSortRequirement());
}

TEST_F(CanonicalDistinctTest, FailsToParseDistinctWithUnknownFields) {
    BSONObj cmdObj = fromjson(R"({
        distinct: 'testcoll',
        key: "a",
        $db: 'testdb',
        unknown: 1
    })");

    ASSERT_THROWS_CODE(
        bsonToParsedDistinct(expCtx, cmdObj), DBException, ErrorCodes::IDLUnknownField);
}

TEST_F(CanonicalDistinctTest, FailsToParseDistinctWithMissingKey) {
    BSONObj cmdObj = fromjson(R"({
        distinct: 'testns',
        $db: 'testdb'
    })");

    ASSERT_THROWS_CODE(
        bsonToParsedDistinct(expCtx, cmdObj), DBException, ErrorCodes::IDLFailedToParse);
}

TEST_F(CanonicalDistinctTest, FailsToParseDistinctWithInvalidKeyType) {
    BSONObj cmdObj = fromjson(R"({
        distinct: 'testns',
        key: {a: 1},
        $db: 'test'
    })");

    ASSERT_THROWS_CODE(bsonToParsedDistinct(expCtx, cmdObj), DBException, ErrorCodes::TypeMismatch);
}

TEST_F(CanonicalDistinctTest, FailsToParseDistinctWithInvalidGenericCommandArgument) {
    BSONObj cmdObj = fromjson(R"({
        distinct: 'testns',
        key: 'a',
        readConcern: {level: 'invalid'},
        $db: 'test'
    })");

    ASSERT_THROWS_CODE(bsonToParsedDistinct(expCtx, cmdObj), DBException, ErrorCodes::BadValue);
}

}  // namespace
}  // namespace mongo
