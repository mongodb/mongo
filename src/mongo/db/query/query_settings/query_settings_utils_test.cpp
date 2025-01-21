/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_manager.h"
#include "mongo/db/query/query_settings/query_settings_utils.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/serialization_context.h"

namespace mongo::query_settings {

static auto const kSerializationContext =
    SerializationContext{SerializationContext::Source::Command,
                         SerializationContext::CallerType::Request,
                         SerializationContext::Prefix::ExcludePrefix};

class QuerySettingsLoookupTest : public ServiceContextTest {
public:
    static constexpr StringData kCollName = "exampleColl"_sd;
    static constexpr StringData kDbName = "foo"_sd;

    void setUp() final {
        QuerySettingsManager::create(getServiceContext(), {}, {});

        _opCtx = cc().makeOperationContext();
        _expCtx = ExpressionContext::makeBlankExpressionContext(opCtx(), {NamespaceString()});
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    boost::intrusive_ptr<ExpressionContext> expCtx() {
        return _expCtx;
    }

    static NamespaceString nss() {
        return NamespaceStringUtil::deserialize(
            boost::none, kDbName, kCollName, kSerializationContext);
    }

    QuerySettingsManager& manager() {
        return QuerySettingsManager::get(opCtx());
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

TEST_F(QuerySettingsLoookupTest, QuerySettingsLookupForFind) {
    auto findCmdStr = "{find: 'exampleColl', '$db': 'foo'}"_sd;
    auto findCmdBSON = fromjson(findCmdStr);
    auto findCmd = query_request_helper::makeFromFindCommandForTests(findCmdBSON, nss());
    auto parsedRequest =
        uassertStatusOK(parsed_find_command::parse(expCtx(), {.findCommand = std::move(findCmd)}));

    // Ensure no settings are returned.
    ASSERT_EQ(query_settings::lookupQuerySettingsForFind(expCtx(), *parsedRequest, nss()),
              query_settings::QuerySettings());

    // Set { queryFramework: 'classic' } settings to 'findCmd' command.
    QuerySettings settings;
    settings.setQueryFramework(QueryFrameworkControlEnum::kForceClassicEngine);

    auto queryShapeHash =
        createRepresentativeInfo(opCtx(), findCmdBSON, boost::none /* tenantId */).queryShapeHash;
    QueryShapeConfiguration conf(queryShapeHash, settings);
    conf.setRepresentativeQuery(findCmdBSON);
    manager().setQueryShapeConfigurations(
        opCtx(), {conf}, LogicalTime(Timestamp(Date_t::now())), boost::none /* tenantId */);

    // Ensure that settings are returned during the lookup, after query settings have been
    // populated.
    ASSERT_EQ(query_settings::lookupQuerySettingsForFind(expCtx(), *parsedRequest, nss()),
              settings);
}

TEST_F(QuerySettingsLoookupTest,
       QuerySettingsLookupForFindWithFailingShapeShouldNotThrowException) {
    FailPointEnableBlock fp("queryShapeCreationException");

    auto findCmdStr =
        "{find: 'exampleColl', projection: {foo: {$meta: 'sortKey'}}, '$db': 'foo'}"_sd;
    auto findCmd = query_request_helper::makeFromFindCommandForTests(fromjson(findCmdStr), nss());
    auto parsedRequest =
        uassertStatusOK(parsed_find_command::parse(expCtx(), {.findCommand = std::move(findCmd)}));

    ASSERT_DOES_NOT_THROW(
        query_settings::lookupQuerySettingsForFind(expCtx(), *parsedRequest, nss()));
    ASSERT_EQ(query_settings::lookupQuerySettingsForFind(expCtx(), *parsedRequest, nss()),
              query_settings::QuerySettings());
}

TEST_F(QuerySettingsLoookupTest, QuerySettingsLookupForAgg) {
    auto aggCmdStr =
        "{aggregate: 'exampleColl', pipeline: [{$match: {_id: 0}}], cursor: {}, '$db': 'foo'}"_sd;
    auto aggCmdBSON = fromjson(aggCmdStr);
    auto aggCmd = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(aggCmdBSON));
    auto pipeline = Pipeline::parse(aggCmd.getPipeline(), expCtx());

    // Ensure no settings are returned.
    ASSERT_EQ(
        query_settings::lookupQuerySettingsForAgg(expCtx(), aggCmd, *pipeline, {nss()}, nss()),
        query_settings::QuerySettings());

    // Set { queryFramework: 'classic' } settings to 'aggCmd' command.
    QuerySettings settings;
    settings.setQueryFramework(QueryFrameworkControlEnum::kForceClassicEngine);

    auto queryShapeHash =
        createRepresentativeInfo(opCtx(), aggCmdBSON, boost::none /* tenantId */).queryShapeHash;
    QueryShapeConfiguration conf(queryShapeHash, settings);
    conf.setRepresentativeQuery(aggCmdBSON);
    manager().setQueryShapeConfigurations(
        opCtx(), {conf}, LogicalTime(Timestamp(Date_t::now())), boost::none /* tenantId */);

    // Ensure that settings are returned during the lookup, after query settings have been
    // populated.
    ASSERT_EQ(
        query_settings::lookupQuerySettingsForAgg(expCtx(), aggCmd, *pipeline, {nss()}, nss()),
        settings);
}

TEST_F(QuerySettingsLoookupTest, QuerySettingsLookupForAggWithFailingShapeShouldNotThrowException) {
    FailPointEnableBlock fp("queryShapeCreationException");

    auto aggCmdStr =
        "{aggregate: 'exampleColl', pipeline: [{$match: {_id: 0}}], cursor: {}, '$db': 'foo'}"_sd;
    auto aggCmdBSON = fromjson(aggCmdStr);
    auto aggCmd = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(aggCmdBSON));
    auto pipeline = Pipeline::parse(aggCmd.getPipeline(), expCtx());

    ASSERT_DOES_NOT_THROW(
        query_settings::lookupQuerySettingsForAgg(expCtx(), aggCmd, *pipeline, {nss()}, nss()));
    ASSERT_EQ(
        query_settings::lookupQuerySettingsForAgg(expCtx(), aggCmd, *pipeline, {nss()}, nss()),
        query_settings::QuerySettings());
}

TEST_F(QuerySettingsLoookupTest, QuerySettingsLookupForDistinct) {
    auto distinctCmdBSON = fromjson("{distinct: 'exampleColl', key: 'x', $db: 'foo'}");
    auto distinctCmd = std::make_unique<DistinctCommandRequest>(
        DistinctCommandRequest::parse(IDLParserContext("distinctCommandRequest",
                                                       false /* apiStrict */,
                                                       auth::ValidatedTenancyScope::get(opCtx()),
                                                       boost::none,
                                                       SerializationContext::stateDefault()),
                                      distinctCmdBSON));
    auto parsedDistinct =
        parsed_distinct_command::parse(expCtx(),
                                       distinctCmdBSON,
                                       std::move(distinctCmd),
                                       ExtensionsCallbackNoop(),
                                       MatchExpressionParser::kAllowAllSpecialFeatures);

    // Ensure no settings are returned.
    ASSERT_EQ(query_settings::lookupQuerySettingsForDistinct(expCtx(), *parsedDistinct, nss()),
              query_settings::QuerySettings());

    // Set { queryFramework: 'classic' } settings to 'distinctCmd' command.
    QuerySettings settings;
    settings.setQueryFramework(QueryFrameworkControlEnum::kForceClassicEngine);

    auto queryShapeHash =
        createRepresentativeInfo(opCtx(), distinctCmdBSON, boost::none /* tenantId */)
            .queryShapeHash;
    QueryShapeConfiguration conf(queryShapeHash, settings);
    conf.setRepresentativeQuery(distinctCmdBSON);
    manager().setQueryShapeConfigurations(
        opCtx(), {conf}, LogicalTime(Timestamp(Date_t::now())), boost::none /* tenantId */);

    // Ensure that settings are returned during the lookup, after query settings have been
    // populated.
    ASSERT_EQ(query_settings::lookupQuerySettingsForDistinct(expCtx(), *parsedDistinct, nss()),
              settings);
}

TEST_F(QuerySettingsLoookupTest,
       QuerySettingsLookupForDistinctWithFailingShapeShouldNotThrowException) {
    FailPointEnableBlock fp("queryShapeCreationException");

    auto distinctCmdBSON = fromjson("{distinct: 'exampleColl', key: 'x', $db: 'foo'}");
    auto distinctCommand = std::make_unique<DistinctCommandRequest>(
        DistinctCommandRequest::parse(IDLParserContext("distinctCommandRequest",
                                                       false /* apiStrict */,
                                                       auth::ValidatedTenancyScope::get(opCtx()),
                                                       boost::none,
                                                       SerializationContext::stateDefault()),
                                      distinctCmdBSON));
    auto parsedDistinct =
        parsed_distinct_command::parse(expCtx(),
                                       distinctCmdBSON,
                                       std::move(distinctCommand),
                                       ExtensionsCallbackNoop(),
                                       MatchExpressionParser::kAllowAllSpecialFeatures);

    ASSERT_DOES_NOT_THROW(
        query_settings::lookupQuerySettingsForDistinct(expCtx(), *parsedDistinct, nss()));
    ASSERT_EQ(query_settings::lookupQuerySettingsForDistinct(expCtx(), *parsedDistinct, nss()),
              query_settings::QuerySettings());
}

}  // namespace mongo::query_settings
