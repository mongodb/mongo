/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
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
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/query/query_shape/agg_cmd_shape.h"
#include "mongo/db/query/query_shape/distinct_cmd_shape.h"
#include "mongo/db/query/query_shape/find_cmd_shape.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/serialization_context.h"

namespace mongo::query_settings {

static auto const kSerializationContext =
    SerializationContext{SerializationContext::Source::Command,
                         SerializationContext::CallerType::Request,
                         SerializationContext::Prefix::ExcludePrefix};

class QuerySettingsScope {
public:
    QuerySettingsScope(OperationContext* opCtx,
                       std::vector<QueryShapeConfiguration> queryShapeConfigurations)
        : _opCtx(opCtx),
          _previousQueryShapeConfigurationsWithTimestamp(
              getAllQueryShapeConfigurations(opCtx, boost::none /* tenantId */)) {
        LogicalTime newTime = _previousQueryShapeConfigurationsWithTimestamp.clusterParameterTime;
        newTime.addTicks(1);

        setAllQueryShapeConfigurations(
            _opCtx,
            QueryShapeConfigurationsWithTimestamp{queryShapeConfigurations, newTime},
            boost::none /* tenantId */);
    }

    ~QuerySettingsScope() {
        setAllQueryShapeConfigurations(_opCtx,
                                       std::move(_previousQueryShapeConfigurationsWithTimestamp),
                                       boost::none /* tenantId */);
    }

private:
    OperationContext* _opCtx;
    QueryShapeConfigurationsWithTimestamp _previousQueryShapeConfigurationsWithTimestamp;
};

class InternalClientScope {
public:
    explicit InternalClientScope(OperationContext* opCtx)
        : _opCtx(opCtx), _wasInternalClient(opCtx->getClient()->isInternalClient()) {
        _opCtx->getClient()->setIsInternalClient(true);
    }

    ~InternalClientScope() {
        _opCtx->getClient()->setIsInternalClient(_wasInternalClient);
    }

private:
    OperationContext* _opCtx;
    bool _wasInternalClient;
};

class ExplainScope {
public:
    ExplainScope(boost::intrusive_ptr<ExpressionContext> expCtx, const BSONObj& originalCmd)
        : _expCtx(expCtx),
          _explainCmdBSON(BSON("explain" << originalCmd)),
          _previousExplain(_expCtx->getExplain()) {
        _expCtx->setExplain(ExplainOptions::Verbosity::kQueryPlanner);
    }

    BSONObj explainCmd() const {
        return _explainCmdBSON;
    }

    ~ExplainScope() {
        _expCtx->setExplain(_previousExplain);
    }

private:
    boost::intrusive_ptr<ExpressionContext> _expCtx;
    BSONObj _explainCmdBSON;
    boost::optional<ExplainOptions::Verbosity> _previousExplain;
};

class QuerySettingsLoookupTest : public ServiceContextTest {
public:
    static constexpr StringData kCollName = "exampleColl"_sd;
    static constexpr StringData kDbName = "foo"_sd;

    void setUp() final {
        // Initialize the query settings.
        initializeForTest(getServiceContext());

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

    QueryShapeConfiguration makeQueryShapeConfiguration(const BSONObj& cmdBSON,
                                                        const QuerySettings& querySettings) {
        auto queryShapeHash =
            createRepresentativeInfo(opCtx(), cmdBSON, boost::none /* tenantId */).queryShapeHash;
        QueryShapeConfiguration config(queryShapeHash, querySettings);
        config.setRepresentativeQuery(cmdBSON);
        return config;
    }

    void assertQuerySettingsLookup(const BSONObj& cmdBSON,
                                   const query_shape::DeferredQueryShape& deferredShape,
                                   const NamespaceString& nss) {
        assertQuerySettingsLookupWithoutRejectionCheck(cmdBSON, deferredShape, nss);
        assertQuerySettingsLookupWithRejectionCheck(cmdBSON, deferredShape, nss);
    }

    /**
     * Ensures that QuerySettings lookup returns the correct settings for the corresponding query.
     */
    void assertQuerySettingsLookupWithoutRejectionCheck(
        const BSONObj& cmdBSON,
        const query_shape::DeferredQueryShape& deferredShape,
        const NamespaceString& nss) {
        const bool isExplain = cmdBSON.firstElementFieldNameStringData() == "explain";
        BSONObj cmdForSettingsBSON = isExplain ? cmdBSON.firstElement().Obj() : cmdBSON;

        QuerySettings forceClassicEngineSettings;
        forceClassicEngineSettings.setQueryFramework(
            QueryFrameworkControlEnum::kForceClassicEngine);
        QuerySettings useSbeEngineSettings;
        useSbeEngineSettings.setQueryFramework(QueryFrameworkControlEnum::kTrySbeEngine);

        // Ensure empty settings are returned if no settings are present in the system.
        ASSERT_EQ(lookupQuerySettingsWithRejectionCheckOnRouter(expCtx(), deferredShape, nss),
                  QuerySettings());
        ASSERT_EQ(lookupQuerySettingsWithRejectionCheckOnShard(
                      expCtx(), deferredShape, nss, QuerySettings()),
                  QuerySettings());

        // Set { queryFramework: 'classic' } settings to 'cmdForSettingsBSON'.
        QuerySettingsScope forceClassicEngineQuerySettingsScope(
            opCtx(), {makeQueryShapeConfiguration(cmdForSettingsBSON, forceClassicEngineSettings)});

        // Ensure that 'forceClassicEngineSettings' are returned during the lookup, after query
        // settings have been populated.
        ASSERT_EQ(lookupQuerySettingsWithRejectionCheckOnRouter(expCtx(), deferredShape, nss),
                  forceClassicEngineSettings);

        // Ensure that in case of a replica set case, a regular query settings lookup is performed.
        ASSERT_EQ(
            lookupQuerySettingsWithRejectionCheckOnShard(
                expCtx(), deferredShape, nss, boost::none /* querySettingsFromOriginalCommand */),
            forceClassicEngineSettings);

        // Simulate performing QuerySettings lookup on the shard in sharded cluster.
        {
            InternalClientScope internalClientScope(opCtx());

            // Ensure that settings passed to the method are being returned as opposed to performing
            // the QuerySettings lookup.
            ASSERT_EQ(lookupQuerySettingsWithRejectionCheckOnShard(
                          expCtx(), deferredShape, nss, useSbeEngineSettings),
                      useSbeEngineSettings);

            // Ensure that empty settings are returned if original command did not have any settings
            // specified as opposed to performing QuerySettings lookup.
            ASSERT_EQ(lookupQuerySettingsWithRejectionCheckOnShard(
                          expCtx(),
                          deferredShape,
                          nss,
                          boost::none /* querySettingsFromOriginalCommand */),
                      QuerySettings());
        }
    }

    /**
     * Ensures that QuerySettings lookup rejects the queries if the associated settings contain
     * 'reject: true'.
     */
    void assertQuerySettingsLookupWithRejectionCheck(
        const BSONObj& cmdBSON,
        const query_shape::DeferredQueryShape& deferredShape,
        const NamespaceString& nss) {
        const bool isExplain = cmdBSON.firstElementFieldNameStringData() == "explain";
        BSONObj cmdForSettingsBSON = isExplain ? cmdBSON.firstElement().Obj() : cmdBSON;

        // Set { reject: true } settings to 'cmdForSettingsBSON'.
        QuerySettings querySettingsWithReject;
        querySettingsWithReject.setReject(true);
        QuerySettingsScope rejectQuerySettingsScope(
            opCtx(), {makeQueryShapeConfiguration(cmdForSettingsBSON, querySettingsWithReject)});

        // Ensure query is not rejected if an explain query is run, otherwise is rejected by
        // throwing an exception with QueryRejectedBySettings error code.
        if (isExplain) {
            ASSERT_DOES_NOT_THROW(
                lookupQuerySettingsWithRejectionCheckOnRouter(expCtx(), deferredShape, nss));
            ASSERT_DOES_NOT_THROW(lookupQuerySettingsWithRejectionCheckOnShard(
                expCtx(), deferredShape, nss, boost::none /* querySettingsFromOriginalCommand */));
        } else {
            ASSERT_THROWS_CODE(
                lookupQuerySettingsWithRejectionCheckOnRouter(expCtx(), deferredShape, nss),
                DBException,
                ErrorCodes::QueryRejectedBySettings);
            ASSERT_THROWS_CODE(lookupQuerySettingsWithRejectionCheckOnShard(
                                   expCtx(),
                                   deferredShape,
                                   nss,
                                   boost::none /* querySettingsFromOriginalCommand */),
                               DBException,
                               ErrorCodes::QueryRejectedBySettings);
        }

        // Ensure query is not rejected on the shard in sharded cluster.
        {
            InternalClientScope internalClientScope(opCtx());
            ASSERT_DOES_NOT_THROW(lookupQuerySettingsWithRejectionCheckOnShard(
                expCtx(), deferredShape, nss, querySettingsWithReject));
            ASSERT_DOES_NOT_THROW(lookupQuerySettingsWithRejectionCheckOnShard(
                expCtx(), deferredShape, nss, boost::none /* querySettingsFromOriginalCommand */));
        }
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
    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::FindCmdShape>(*parsedRequest, expCtx());
    }};

    assertQuerySettingsLookup(findCmdBSON, deferredShape, nss());
    {
        ExplainScope explainScope(expCtx(), findCmdBSON);
        assertQuerySettingsLookup(explainScope.explainCmd(), deferredShape, nss());
    }
}

TEST_F(QuerySettingsLoookupTest, QuerySettingsLookupForAgg) {
    auto aggCmdStr =
        "{aggregate: 'exampleColl', pipeline: [{$match: {_id: 0}}], cursor: {}, '$db': 'foo'}"_sd;
    auto aggCmdBSON = fromjson(aggCmdStr);
    auto aggCmd = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(aggCmdBSON));
    auto pipeline = Pipeline::parse(aggCmd.getPipeline(), expCtx());
    stdx::unordered_set<NamespaceString> involvedNamespaces = {nss()};
    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::AggCmdShape>(
            aggCmd, nss(), involvedNamespaces, *pipeline, expCtx());
    }};

    assertQuerySettingsLookup(aggCmdBSON, deferredShape, nss());
    {
        ExplainScope explainScope(expCtx(), aggCmdBSON);
        assertQuerySettingsLookup(explainScope.explainCmd(), deferredShape, nss());
    }
}

TEST_F(QuerySettingsLoookupTest, QuerySettingsLookupForDistinct) {
    auto distinctCmdBSON = fromjson("{distinct: 'exampleColl', key: 'x', $db: 'foo'}");
    auto distinctCmd = std::make_unique<DistinctCommandRequest>(
        DistinctCommandRequest::parse(IDLParserContext("distinctCommandRequest",
                                                       auth::ValidatedTenancyScope::get(opCtx()),
                                                       boost::none,
                                                       SerializationContext::stateDefault()),
                                      distinctCmdBSON));
    auto parsedDistinct =
        parsed_distinct_command::parse(expCtx(),
                                       std::move(distinctCmd),
                                       ExtensionsCallbackNoop(),
                                       MatchExpressionParser::kAllowAllSpecialFeatures);
    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::DistinctCmdShape>(*parsedDistinct,
                                                                          expCtx());
    }};

    assertQuerySettingsLookup(distinctCmdBSON, deferredShape, nss());
    {
        ExplainScope explainScope(expCtx(), distinctCmdBSON);
        assertQuerySettingsLookup(explainScope.explainCmd(), deferredShape, nss());
    }
}

}  // namespace mongo::query_settings
