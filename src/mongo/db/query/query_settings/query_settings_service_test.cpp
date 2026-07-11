// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_settings/query_settings_service.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_settings/query_settings.h"
#include "mongo/db/query/query_settings/query_settings_cluster_parameter_gen.h"
#include "mongo/db/query/query_settings/query_settings_context.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_shape/agg_cmd_shape.h"
#include "mongo/db/query/query_shape/distinct_cmd_shape.h"
#include "mongo/db/query/query_shape/find_cmd_shape.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/serialization_context.h"

#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::query_settings {
static bool operator==(const QueryShapeConfiguration& lhs, const QueryShapeConfiguration& rhs) {
    return SimpleBSONObjComparator::kInstance.compare(lhs.toBSON(), rhs.toBSON()) == 0;
}

static bool operator==(const QueryShapeConfigurationsWithTimestamp& lhs,
                       const QueryShapeConfigurationsWithTimestamp& rhs) {
    return lhs.clusterParameterTime == rhs.clusterParameterTime &&
        lhs.queryShapeConfigurations == rhs.queryShapeConfigurations;
}

namespace {
using namespace std::literals::string_view_literals;
using namespace query_settings_details;
static auto const kSerializationContext =
    SerializationContext{SerializationContext::Source::Command,
                         SerializationContext::CallerType::Request,
                         SerializationContext::Prefix::ExcludePrefix};

BSONObj makeSettingsClusterParameter(const QueryShapeConfigurationsWithTimestamp& config) {
    BSONArrayBuilder settings;
    for (const auto& queryShapeConfig : config.queryShapeConfigurations) {
        settings.append(queryShapeConfig.toBSON());
    }
    return BSON("_id" << QuerySettingsService::getQuerySettingsClusterParameterName()
                      << QuerySettingsClusterParameterValue::kSettingsArrayFieldName
                      << settings.arr()
                      << QuerySettingsClusterParameterValue::kClusterParameterTimeFieldName
                      << config.clusterParameterTime.asTimestamp());
}

BSONObj makeSettingsClusterParameter(
    const BSONArray& settings, LogicalTime clusterParameterTime = LogicalTime(Timestamp(113, 59))) {
    return BSON("_id" << QuerySettingsService::getQuerySettingsClusterParameterName()
                      << QuerySettingsClusterParameterValue::kSettingsArrayFieldName << settings
                      << QuerySettingsClusterParameterValue::kClusterParameterTimeFieldName
                      << clusterParameterTime.asTimestamp());
}

QuerySettings makeQuerySettings(const IndexHintSpecs& indexHints, bool setFramework = true) {
    QuerySettings settings;
    if (!indexHints.empty()) {
        settings.setIndexHints(indexHints);
    }
    if (setFramework) {
        settings.setQueryFramework(mongo::QueryFrameworkControlEnum::kTrySbeEngine);
    }
    return settings;
}

auto makeDbName(std::string_view dbName) {
    return DatabaseNameUtil::deserialize(
        boost::none /*tenantId=*/, dbName, SerializationContext::stateDefault());
}

NamespaceSpec makeNsSpec(std::string_view collName) {
    NamespaceSpec ns;
    ns.setDb(makeDbName("testDbA"));
    ns.setColl(collName);
    return ns;
}

class QuerySettingsScope {
public:
    QuerySettingsScope(OperationContext* opCtx,
                       std::vector<QueryShapeConfiguration> queryShapeConfigurations)
        : _opCtx(opCtx),
          _previousQueryShapeConfigurationsWithTimestamp(
              QuerySettingsService::get(opCtx).getAllQueryShapeConfigurations(
                  boost::none /* tenantId */)) {
        LogicalTime newTime = _previousQueryShapeConfigurationsWithTimestamp.clusterParameterTime;
        newTime.addTicks(1);

        QuerySettingsService::get(_opCtx).setAllQueryShapeConfigurations(
            QueryShapeConfigurationsWithTimestamp{queryShapeConfigurations, newTime},
            boost::none /* tenantId */);
    }

    ~QuerySettingsScope() {
        _previousQueryShapeConfigurationsWithTimestamp.clusterParameterTime.addTicks(1);
        QuerySettingsService::get(_opCtx).setAllQueryShapeConfigurations(
            std::move(_previousQueryShapeConfigurationsWithTimestamp), boost::none /* tenantId */);
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

class QuerySettingsServiceTest : public ServiceContextTest {
public:
    static constexpr std::string_view kCollName = "exampleColl"sv;
    static constexpr std::string_view kDbName = "foo"sv;

    void setUp() final {
        // Initialize the query settings.
        _opCtx = cc().makeOperationContext();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    boost::intrusive_ptr<ExpressionContext> expCtx() {
        if (!_expCtx) {
            _expCtx = makeBlankExpressionContext(opCtx(), {NamespaceString()});
        }

        return _expCtx;
    }

    QuerySettingsService& service() {
        return QuerySettingsService::get(opCtx());
    }

    static NamespaceString nss() {
        return NamespaceStringUtil::deserialize(
            boost::none, kDbName, kCollName, kSerializationContext);
    }

    query_shape::QueryShapeHash hashForShape(const query_shape::DeferredQueryShape& shape) {
        return shape().getValue()->sha256Hash(opCtx(), kSerializationContext);
    }

    QueryShapeConfiguration makeQueryShapeConfiguration(
        const BSONObj& cmdBSON,
        const QuerySettings& querySettings,
        boost::optional<TenantId> tenantId = boost::none) {
        auto queryShapeHash = createRepresentativeInfo(opCtx(), cmdBSON, tenantId).queryShapeHash;
        QueryShapeConfiguration config(queryShapeHash, querySettings);
        config.setRepresentativeQuery(cmdBSON);
        return config;
    }

    std::vector<QueryShapeConfiguration> getExampleQueryShapeConfigurations(TenantId tenantId) {
        NamespaceSpec ns;
        ns.setDb(DatabaseNameUtil::deserialize(tenantId, kDbName, kSerializationContext));
        ns.setColl(kCollName);

        const QuerySettings settings = makeQuerySettings({IndexHintSpec(ns, {IndexHint("a_1")})});
        QueryInstance queryA =
            BSON("find" << kCollName << "$db" << kDbName << "filter" << BSON("a" << 2));
        QueryInstance queryB =
            BSON("find" << kCollName << "$db" << kDbName << "filter" << BSON("a" << BSONNULL));
        return {makeQueryShapeConfiguration(queryA, settings, tenantId),
                makeQueryShapeConfiguration(queryB, settings, tenantId)};
    }

    void assertQuerySettingsLookup(const BSONObj& cmdBSON,
                                   const query_shape::DeferredQueryShape& deferredShape,
                                   const NamespaceString& nss) {
        {
            QuerySettingsService::initializeForRouter(getServiceContext());
            assertQuerySettingsLookupWithoutRejectionCheckForRouter(cmdBSON, deferredShape, nss);
            assertQuerySettingsLookupWithRejectionCheckForRouter(cmdBSON, deferredShape, nss);
        }

        {
            QuerySettingsService::initializeForShard(getServiceContext(),
                                                     nullptr /* setClusterParameterImplFn */);
            assertQuerySettingsLookupWithoutRejectionCheckForShard(cmdBSON, deferredShape, nss);
            assertQuerySettingsLookupWithRejectionCheckForShard(cmdBSON, deferredShape, nss);
        }
    }

    /**
     * Ensures that QuerySettings lookup returns the correct settings for the corresponding query.
     */
    void assertQuerySettingsLookupWithoutRejectionCheckForRouter(
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
        ASSERT_EQ(service().lookupQuerySettingsWithRejectionCheck(
                      expCtx(), hashForShape(deferredShape), nss, boost::none),
                  QuerySettings());

        // Set { queryFramework: 'classic' } settings to 'cmdForSettingsBSON'.
        QuerySettingsScope forceClassicEngineQuerySettingsScope(
            opCtx(), {makeQueryShapeConfiguration(cmdForSettingsBSON, forceClassicEngineSettings)});

        // Ensure that 'forceClassicEngineSettings' are returned during the lookup, after query
        // settings have been populated.
        ASSERT_EQ(service().lookupQuerySettingsWithRejectionCheck(
                      expCtx(), hashForShape(deferredShape), nss, boost::none),
                  forceClassicEngineSettings);
    }

    /**
     * Ensures that QuerySettings lookup returns the correct settings for the corresponding query.
     */
    void assertQuerySettingsLookupWithoutRejectionCheckForShard(
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
        ASSERT_EQ(service().lookupQuerySettingsWithRejectionCheck(
                      expCtx(), hashForShape(deferredShape), nss, boost::none),
                  QuerySettings());

        // Set { queryFramework: 'classic' } settings to 'cmdForSettingsBSON'.
        QuerySettingsScope forceClassicEngineQuerySettingsScope(
            opCtx(), {makeQueryShapeConfiguration(cmdForSettingsBSON, forceClassicEngineSettings)});

        // Ensure that in case of a replica set case, a regular query settings lookup is performed.
        ASSERT_EQ(service().lookupQuerySettingsWithRejectionCheck(
                      expCtx(),
                      hashForShape(deferredShape),
                      nss,
                      boost::none /* querySettingsFromOriginalCommand */),
                  forceClassicEngineSettings);

        // Simulate performing QuerySettings lookup on the shard in sharded cluster.
        {
            InternalClientScope internalClientScope(opCtx());

            // Ensure that settings passed to the method are being returned as opposed to performing
            // the QuerySettings lookup.
            ASSERT_EQ(service().lookupQuerySettingsWithRejectionCheck(
                          expCtx(), hashForShape(deferredShape), nss, useSbeEngineSettings),
                      useSbeEngineSettings);

            // Ensure that empty settings are returned if original command did not have any settings
            // specified as opposed to performing QuerySettings lookup.
            ASSERT_EQ(service().lookupQuerySettingsWithRejectionCheck(
                          expCtx(),
                          hashForShape(deferredShape),
                          nss,
                          boost::none /* querySettingsFromOriginalCommand */),
                      QuerySettings());
        }
    }

    /**
     * Ensures that QuerySettings lookup rejects the queries if the associated settings contain
     * 'reject: true'.
     */
    void assertQuerySettingsLookupWithRejectionCheckForRouter(
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
            ASSERT_DOES_NOT_THROW(service().lookupQuerySettingsWithRejectionCheck(
                expCtx(), hashForShape(deferredShape), nss, boost::none));
        } else {
            ASSERT_THROWS_CODE(service().lookupQuerySettingsWithRejectionCheck(
                                   expCtx(), hashForShape(deferredShape), nss, boost::none),
                               DBException,
                               ErrorCodes::QueryRejectedBySettings);
        }
    }

    /**
     * Ensures that QuerySettings lookup rejects the queries if the associated settings contain
     * 'reject: true'.
     */
    void assertQuerySettingsLookupWithRejectionCheckForShard(
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
            ASSERT_DOES_NOT_THROW(service().lookupQuerySettingsWithRejectionCheck(
                expCtx(),
                hashForShape(deferredShape),
                nss,
                boost::none /* querySettingsFromOriginalCommand */));
        } else {
            ASSERT_THROWS_CODE(service().lookupQuerySettingsWithRejectionCheck(
                                   expCtx(),
                                   hashForShape(deferredShape),
                                   nss,
                                   boost::none /* querySettingsFromOriginalCommand */),
                               DBException,
                               ErrorCodes::QueryRejectedBySettings);
        }

        // Ensure query is not rejected on the shard in sharded cluster.
        {
            InternalClientScope internalClientScope(opCtx());
            ASSERT_DOES_NOT_THROW(service().lookupQuerySettingsWithRejectionCheck(
                expCtx(), hashForShape(deferredShape), nss, querySettingsWithReject));
            ASSERT_DOES_NOT_THROW(service().lookupQuerySettingsWithRejectionCheck(
                expCtx(),
                hashForShape(deferredShape),
                nss,
                boost::none /* querySettingsFromOriginalCommand */));
        }
    }

    void assertSanitizeInvalidIndexHints(const IndexHintSpecs& initialSpec,
                                         const IndexHintSpecs& expectedSpec) {
        QueryInstance query = BSON("find" << "exampleColl"
                                          << "$db"
                                          << "foo");
        auto initSettings = makeQueryShapeConfiguration(query, makeQuerySettings(initialSpec));
        std::vector<QueryShapeConfiguration> queryShapeConfigs{initSettings};
        const auto expectedSettings = makeQuerySettings(expectedSpec);
        ASSERT_DOES_NOT_THROW(service().sanitizeQuerySettingsHints(queryShapeConfigs));
        ASSERT_BSONOBJ_EQ(queryShapeConfigs[0].getSettings().toBSON(), expectedSettings.toBSON());
    }


private:
    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContext> _expCtx;

    // Disable the query settings backfill for the duration of this test.
    unittest::ServerParameterGuard disableBackfillGuard{"internalQuerySettingsDisableBackfill",
                                                        true};
};

TEST_F(QuerySettingsServiceTest, QuerySettingsLookupForFind) {
    auto findCmdStr = "{find: 'exampleColl', '$db': 'foo'}"sv;
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

TEST_F(QuerySettingsServiceTest, QuerySettingsLookupForAgg) {
    auto aggCmdStr =
        "{aggregate: 'exampleColl', pipeline: [{$match: {_id: 0}}], cursor: {}, '$db': 'foo'}"sv;
    auto aggCmdBSON = fromjson(aggCmdStr);
    auto aggCmd = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(aggCmdBSON));
    auto pipeline = pipeline_factory::makePipeline(
        aggCmd.getPipeline(), expCtx(), pipeline_factory::kOptionsMinimal);
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

TEST_F(QuerySettingsServiceTest, QuerySettingsLookupForDistinct) {
    auto distinctCmdBSON = fromjson("{distinct: 'exampleColl', key: 'x', $db: 'foo'}");
    auto distinctCmd = std::make_unique<DistinctCommandRequest>(
        DistinctCommandRequest::parse(distinctCmdBSON,
                                      IDLParserContext("distinctCommandRequest",
                                                       auth::ValidatedTenancyScope::get(opCtx()),
                                                       boost::none,
                                                       SerializationContext::stateDefault())));
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

TEST_F(QuerySettingsServiceTest, InitializeSettingsForQueryIsNoOpWhenNotStarted) {
    QuerySettingsService::initializeForTest(getServiceContext());

    // No eligible command has begun on the operation; its settings read as default.
    ASSERT(std::holds_alternative<NotStarted>(getQuerySettingsStateForOp(opCtx())));
    ASSERT_EQ(forOp(opCtx()), QuerySettings());

    QuerySettings settings;
    settings.setQueryFramework(QueryFrameworkControlEnum::kForceClassicEngine);

    // Resolution is skipped while no eligible command has begun, so the state and default settings
    // are unchanged even though non-default settings were supplied.
    service().initializeSettingsForQuery(expCtx(), boost::none, nss(), settings);

    ASSERT(std::holds_alternative<NotStarted>(getQuerySettingsStateForOp(opCtx())));
    ASSERT_EQ(forOp(opCtx()), QuerySettings());
}

TEST_F(QuerySettingsServiceTest, InitializeSettingsForQueryWithoutHashUsesOriginalCommandSettings) {
    QuerySettingsService::initializeForTest(getServiceContext());
    getQuerySettingsStateForOp(opCtx()) = Pending{};

    QuerySettings settings;
    settings.setQueryFramework(QueryFrameworkControlEnum::kForceClassicEngine);

    // No query shape hash (query ineligible for lookup): settings are taken from those forwarded on
    // the original command.
    service().initializeSettingsForQuery(expCtx(), boost::none, nss(), settings);

    ASSERT(std::holds_alternative<QuerySettings>(getQuerySettingsStateForOp(opCtx())));
    ASSERT_EQ(forOp(opCtx()), settings);
}

TEST_F(QuerySettingsServiceTest, InitializeSettingsForQueryWithoutHashOrCommandResolvesToDefault) {
    QuerySettingsService::initializeForTest(getServiceContext());
    getQuerySettingsStateForOp(opCtx()) = Pending{};

    service().initializeSettingsForQuery(expCtx(), boost::none, nss(), boost::none);

    // A default resolution collapses to 'Empty'.
    ASSERT(std::holds_alternative<Empty>(getQuerySettingsStateForOp(opCtx())));
    ASSERT_EQ(forOp(opCtx()), QuerySettings());
}

TEST_F(QuerySettingsServiceTest, InitializeSettingsForQueryIsNoOpOnceResolved) {
    QuerySettingsService::initializeForTest(getServiceContext());
    getQuerySettingsStateForOp(opCtx()) = Pending{};

    QuerySettings first;
    first.setQueryFramework(QueryFrameworkControlEnum::kForceClassicEngine);
    QuerySettings second;
    second.setQueryFramework(QueryFrameworkControlEnum::kTrySbeEngine);

    service().initializeSettingsForQuery(expCtx(), boost::none, nss(), first);
    // A re-entrant resolution (view re-dispatch, nested direct-client query) must not overwrite the
    // settings already resolved for the outer query.
    service().initializeSettingsForQuery(expCtx(), boost::none, nss(), second);

    ASSERT(std::holds_alternative<QuerySettings>(getQuerySettingsStateForOp(opCtx())));
    ASSERT_EQ(forOp(opCtx()), first);
}

/**
 * Tests that valid index hint specs are the same before and after index hint sanitization.
 */
TEST_F(QuerySettingsServiceTest, ValidIndexHintsAreTheSameBeforeAndAfterSanitization) {
    IndexHintSpecs indexHintSpec{IndexHintSpec(
        makeNsSpec("testCollA"sv), {IndexHint(BSON("a" << 1)), IndexHint(BSON("b" << -1.0))})};
    assertSanitizeInvalidIndexHints(indexHintSpec, indexHintSpec);
}

/**
 * Tests that invalid key-pattern are removed after sanitization.
 */
TEST_F(QuerySettingsServiceTest, InvalidKeyPatternIndexesAreRemovedAfterSanitization) {
    IndexHintSpecs indexHintSpec{IndexHintSpec(makeNsSpec("testCollA"sv),
                                               {IndexHint(BSON("a" << 1 << "c"
                                                                   << "invalid")),
                                                IndexHint(BSON("b" << -1.0))})};
    IndexHintSpecs expectedHintSpec{
        IndexHintSpec(makeNsSpec("testCollA"sv), {IndexHint(BSON("b" << -1.0))})};
    assertSanitizeInvalidIndexHints(indexHintSpec, expectedHintSpec);
}

/**
 * Same as the above test but with more complex examples.
 */
TEST_F(QuerySettingsServiceTest, InvalidKeyPatternIndexesAreRemovedAfterSanitizationComplex) {
    IndexHintSpecs indexHintSpec{IndexHintSpec(makeNsSpec("testCollA"sv),
                                               {
                                                   IndexHint(BSON("a" << 1 << "c"
                                                                      << "invalid")),
                                                   IndexHint(BSON("b" << -1.0)),
                                                   IndexHint(BSON("c" << -2.0 << "b" << 4)),
                                                   IndexHint("index_name"),
                                                   IndexHint(BSON("$natural" << 1)),
                                                   IndexHint(BSON("$natural" << -1 << "a" << 2)),
                                                   IndexHint(BSON("a" << -1 << "$natural" << 1)),
                                               })};
    IndexHintSpecs expectedHintSpec{IndexHintSpec(makeNsSpec("testCollA"sv),
                                                  {IndexHint(BSON("b" << -1.0)),
                                                   IndexHint(BSON("c" << -2.0 << "b" << 4)),
                                                   IndexHint("index_name")})};
    assertSanitizeInvalidIndexHints(indexHintSpec, expectedHintSpec);
}

/**
 * Tests that invalid key-pattern are removed after sanitization and the resulted index hints are
 * empty.
 */
TEST_F(QuerySettingsServiceTest, InvalidKeyPatternIndexesAreRemovedAfterSanitizationEmptyHints) {
    IndexHintSpecs indexHintSpec{IndexHintSpec(makeNsSpec("testCollA"sv),
                                               {
                                                   IndexHint(BSON("a" << 1 << "c"
                                                                      << "invalid")),
                                               })};
    IndexHintSpecs expectedHintSpec;
    assertSanitizeInvalidIndexHints(indexHintSpec, expectedHintSpec);
}

/**
 * Tests that cluster PQS wins over user-supplied querySettings on conflict, and that user settings
 * fill gaps where cluster has no opinion. Tests both router and shard service.
 */
TEST_F(QuerySettingsServiceTest, MergeClusterAndUserQuerySettingsOnRouter) {
    QuerySettingsService::initializeForRouter(getServiceContext());

    auto findCmdBSON = fromjson("{find: 'exampleColl', '$db': 'foo'}");
    auto findCmd = query_request_helper::makeFromFindCommandForTests(findCmdBSON, nss());
    auto parsedRequest =
        uassertStatusOK(parsed_find_command::parse(expCtx(), {.findCommand = std::move(findCmd)}));
    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::FindCmdShape>(*parsedRequest, expCtx());
    }};

    // Set cluster PQS to force classic engine.
    QuerySettings forceClassicEngineSettings;
    forceClassicEngineSettings.setQueryFramework(QueryFrameworkControlEnum::kForceClassicEngine);
    QuerySettingsScope classicScope(
        opCtx(), {makeQueryShapeConfiguration(findCmdBSON, forceClassicEngineSettings)});

    // Conflict: user requests SBE, cluster requests classic -> cluster wins.
    QuerySettings useSbeEngineSettings;
    useSbeEngineSettings.setQueryFramework(QueryFrameworkControlEnum::kTrySbeEngine);
    ASSERT_EQ(service().lookupQuerySettingsWithRejectionCheck(
                  expCtx(), hashForShape(deferredShape), nss(), useSbeEngineSettings),
              forceClassicEngineSettings);

    // With boost::none, cluster settings should be returned as-is.
    ASSERT_EQ(service().lookupQuerySettingsWithRejectionCheck(
                  expCtx(), hashForShape(deferredShape), nss(), boost::none),
              forceClassicEngineSettings);
}

TEST_F(QuerySettingsServiceTest, MergeClusterAndUserQuerySettingsOnShard) {
    QuerySettingsService::initializeForShard(getServiceContext(),
                                             nullptr /* setClusterParameterImplFn */);

    auto findCmdBSON = fromjson("{find: 'exampleColl', '$db': 'foo'}");
    auto findCmd = query_request_helper::makeFromFindCommandForTests(findCmdBSON, nss());
    auto parsedRequest =
        uassertStatusOK(parsed_find_command::parse(expCtx(), {.findCommand = std::move(findCmd)}));
    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::FindCmdShape>(*parsedRequest, expCtx());
    }};

    // Set cluster PQS to force classic engine.
    QuerySettings forceClassicEngineSettings;
    forceClassicEngineSettings.setQueryFramework(QueryFrameworkControlEnum::kForceClassicEngine);
    QuerySettingsScope classicScope(
        opCtx(), {makeQueryShapeConfiguration(findCmdBSON, forceClassicEngineSettings)});

    // Conflict: user requests SBE, cluster requests classic -> cluster wins (standalone shard).
    QuerySettings useSbeEngineSettings;
    useSbeEngineSettings.setQueryFramework(QueryFrameworkControlEnum::kTrySbeEngine);
    ASSERT_EQ(service().lookupQuerySettingsWithRejectionCheck(
                  expCtx(), hashForShape(deferredShape), nss(), useSbeEngineSettings),
              forceClassicEngineSettings);

    // With boost::none, cluster settings should be returned (standalone shard fallback).
    ASSERT_EQ(service().lookupQuerySettingsWithRejectionCheck(
                  expCtx(), hashForShape(deferredShape), nss(), boost::none),
              forceClassicEngineSettings);
}

// User-supplied querySettings are validated during lookup before being merged and applied.
TEST_F(QuerySettingsServiceTest, LookupValidatesUserSuppliedQuerySettings) {
    QuerySettingsService::initializeForRouter(getServiceContext());

    auto findCmdBSON = fromjson("{find: 'exampleColl', '$db': 'foo'}");
    auto findCmd = query_request_helper::makeFromFindCommandForTests(findCmdBSON, nss());
    auto parsedRequest =
        uassertStatusOK(parsed_find_command::parse(expCtx(), {.findCommand = std::move(findCmd)}));
    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::FindCmdShape>(*parsedRequest, expCtx());
    }};
    const auto hash = hashForShape(deferredShape);

    // Unlike setQuerySettings, empty/default user settings are a no-op rather than an error.
    ASSERT_EQ(
        service().lookupQuerySettingsWithRejectionCheck(expCtx(), hash, nss(), QuerySettings()),
        QuerySettings());

    // Two index hints for the same collection are rejected.
    QuerySettings duplicateHints =
        makeQuerySettings({IndexHintSpec(makeNsSpec("testCollA"sv), {IndexHint(BSON("a" << 1))}),
                           IndexHintSpec(makeNsSpec("testCollA"sv), {IndexHint(BSON("b" << 1))})},
                          false /* setFramework */);
    ASSERT_THROWS_CODE(
        service().lookupQuerySettingsWithRejectionCheck(expCtx(), hash, nss(), duplicateHints),
        DBException,
        7746608);
}

// ---- maxTimeMS query setting tests ----

TEST_F(QuerySettingsServiceTest, IsDefaultReturnsFalseWhenOnlyMaxTimeMSIsSet) {
    QuerySettings settings;
    ASSERT(isDefault(settings));
    settings.setMaxTimeMS(5000);
    ASSERT_FALSE(isDefault(settings));
}

TEST_F(QuerySettingsServiceTest, MergeQuerySettingsMaxTimeMS) {
    // rhs maxTimeMS overrides lhs when present.
    QuerySettings lhs;
    lhs.setMaxTimeMS(1000);
    QuerySettings rhs;
    rhs.setMaxTimeMS(2000);
    auto merged = mergeQuerySettings(lhs, rhs);
    ASSERT_EQ(*merged.getMaxTimeMS(), 2000);

    // maxTimeMS is preserved from lhs when rhs has no maxTimeMS.
    QuerySettings rhsNoMax;
    rhsNoMax.setQueryFramework(QueryFrameworkControlEnum::kForceClassicEngine);
    auto merged2 = mergeQuerySettings(lhs, rhsNoMax);
    ASSERT_EQ(*merged2.getMaxTimeMS(), 1000);

    // maxTimeMS from rhs is applied even when lhs has no maxTimeMS.
    QuerySettings lhsNoMax;
    lhsNoMax.setQueryFramework(QueryFrameworkControlEnum::kForceClassicEngine);
    auto merged3 = mergeQuerySettings(lhsNoMax, rhs);
    ASSERT_EQ(*merged3.getMaxTimeMS(), 2000);
}

TEST_F(QuerySettingsServiceTest, MaxTimeMSFromSettingsIsAppliedOnLookup) {
    QuerySettingsService::initializeForShard(getServiceContext(),
                                             nullptr /* setClusterParameterImplFn */);

    auto findCmdBSON = fromjson("{find: 'exampleColl', '$db': 'foo'}");
    auto findCmd = query_request_helper::makeFromFindCommandForTests(findCmdBSON, nss());
    auto parsedRequest =
        uassertStatusOK(parsed_find_command::parse(expCtx(), {.findCommand = std::move(findCmd)}));
    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::FindCmdShape>(*parsedRequest, expCtx());
    }};

    // Confirm there is no deadline before the lookup.
    ASSERT_EQ(opCtx()->getDeadline(), Date_t::max());

    QuerySettings settingsWithMaxTimeMS;
    settingsWithMaxTimeMS.setMaxTimeMS(60000);
    QuerySettingsScope scope(opCtx(),
                             {makeQueryShapeConfiguration(findCmdBSON, settingsWithMaxTimeMS)});

    service().lookupQuerySettingsWithRejectionCheck(
        expCtx(), hashForShape(deferredShape), nss(), boost::none);

    // A finite deadline must have been set by the lookup.
    ASSERT_LT(opCtx()->getDeadline(), Date_t::max());
}

TEST_F(QuerySettingsServiceTest, MaxTimeMSFromSettingsLoosensExistingDeadline) {
    QuerySettingsService::initializeForShard(getServiceContext(),
                                             nullptr /* setClusterParameterImplFn */);

    auto findCmdBSON = fromjson("{find: 'exampleColl', '$db': 'foo'}");
    auto findCmd = query_request_helper::makeFromFindCommandForTests(findCmdBSON, nss());
    auto parsedRequest =
        uassertStatusOK(parsed_find_command::parse(expCtx(), {.findCommand = std::move(findCmd)}));
    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::FindCmdShape>(*parsedRequest, expCtx());
    }};

    // Set a tight 100ms deadline to simulate a command-level maxTimeMS.
    auto tightDeadline =
        opCtx()->getServiceContext()->getFastClockSource()->now() + Milliseconds(100);
    opCtx()->setDeadlineByDate(tightDeadline, ErrorCodes::MaxTimeMSExpired);
    ASSERT_EQ(opCtx()->getDeadline(), tightDeadline);

    // QS maxTimeMS of 60s should loosen the 100ms deadline.
    QuerySettings settingsWithMaxTimeMS;
    settingsWithMaxTimeMS.setMaxTimeMS(60000);
    QuerySettingsScope scope(opCtx(),
                             {makeQueryShapeConfiguration(findCmdBSON, settingsWithMaxTimeMS)});

    service().lookupQuerySettingsWithRejectionCheck(
        expCtx(), hashForShape(deferredShape), nss(), boost::none);

    // The deadline should now be ~60s from now, i.e. well beyond the original 100ms.
    ASSERT_GT(opCtx()->getDeadline(), tightDeadline);
}

TEST_F(QuerySettingsServiceTest, MaxTimeMSZeroRemovesDeadline) {
    QuerySettingsService::initializeForShard(getServiceContext(),
                                             nullptr /* setClusterParameterImplFn */);

    auto findCmdBSON = fromjson("{find: 'exampleColl', '$db': 'foo'}");
    auto findCmd = query_request_helper::makeFromFindCommandForTests(findCmdBSON, nss());
    auto parsedRequest =
        uassertStatusOK(parsed_find_command::parse(expCtx(), {.findCommand = std::move(findCmd)}));
    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::FindCmdShape>(*parsedRequest, expCtx());
    }};

    // Set an existing deadline.
    opCtx()->setDeadlineByDate(opCtx()->getServiceContext()->getFastClockSource()->now() +
                                   Milliseconds(1000),
                               ErrorCodes::MaxTimeMSExpired);
    ASSERT_LT(opCtx()->getDeadline(), Date_t::max());

    // QS maxTimeMS of 0 should remove the deadline (no timeout).
    QuerySettings settingsWithMaxTimeMSZero;
    settingsWithMaxTimeMSZero.setMaxTimeMS(0);
    QuerySettingsScope scope(opCtx(),
                             {makeQueryShapeConfiguration(findCmdBSON, settingsWithMaxTimeMSZero)});

    service().lookupQuerySettingsWithRejectionCheck(
        expCtx(), hashForShape(deferredShape), nss(), boost::none);

    ASSERT_EQ(opCtx()->getDeadline(), Date_t::max());
}

TEST_F(QuerySettingsServiceTest, MaxTimeMSFromSettingsIsNotAppliedForExplain) {
    QuerySettingsService::initializeForShard(getServiceContext(),
                                             nullptr /* setClusterParameterImplFn */);

    auto findCmdBSON = fromjson("{find: 'exampleColl', '$db': 'foo'}");
    auto findCmd = query_request_helper::makeFromFindCommandForTests(findCmdBSON, nss());
    auto parsedRequest =
        uassertStatusOK(parsed_find_command::parse(expCtx(), {.findCommand = std::move(findCmd)}));
    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::FindCmdShape>(*parsedRequest, expCtx());
    }};

    QuerySettings settingsWithMaxTimeMS;
    settingsWithMaxTimeMS.setMaxTimeMS(60000);
    QuerySettingsScope scope(opCtx(),
                             {makeQueryShapeConfiguration(findCmdBSON, settingsWithMaxTimeMS)});

    // Under explain, query settings 'maxTimeMS' must not bound the explain operation itself, so the
    // deadline is left untouched (mirrors how 'reject' exempts explain).
    ExplainScope explainScope(expCtx(), findCmdBSON);
    ASSERT_EQ(opCtx()->getDeadline(), Date_t::max());

    service().lookupQuerySettingsWithRejectionCheck(
        expCtx(), hashForShape(deferredShape), nss(), boost::none);

    ASSERT_EQ(opCtx()->getDeadline(), Date_t::max());
}
}  // namespace
}  // namespace mongo::query_settings
