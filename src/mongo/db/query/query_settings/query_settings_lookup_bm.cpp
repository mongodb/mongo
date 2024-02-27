/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include <benchmark/benchmark.h>

#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_settings/query_settings_manager.h"
#include "mongo/db/query/query_settings/query_settings_utils.h"
#include "mongo/db/query/query_shape/find_cmd_shape.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/processinfo.h"

namespace mongo::query_settings {
namespace {

static auto const kSerializationContext =
    SerializationContext{SerializationContext::Source::Command,
                         SerializationContext::CallerType::Request,
                         SerializationContext::Prefix::Default,
                         true /* nonPrefixedTenantId */};

NamespaceString makeNamespace(const boost::optional<TenantId>& tenantId = boost::none) {
    auto dbName = DatabaseName::createDatabaseName_forTest(tenantId, "db");
    return NamespaceString::createNamespaceString_forTest(dbName, "coll");
}

std::string generateFieldName() {
    return UUID::gen().toString().substr(0, 8);
}

namespace request_generator {

enum class QueryClass : int { kSmall = 0, kMedium, kLarge };

std::string queryClassToString(const QueryClass& complexity) {
    switch (complexity) {
        case QueryClass::kSmall:
            return "small";
        case QueryClass::kMedium:
            return "medium";
        case QueryClass::kLarge:
            return "large";
        default:
            MONGO_UNREACHABLE;
    }
}

// Generates a find command (which belongs to 'small' query class) of the following form:
// {
//   find: <CollName>,
//   $db: <DbName>,
//   filter: { UUID(): { $eq: 1 }}
// }
std::unique_ptr<ParsedFindCommand> generateSmallParsedFindRequest(
    const boost::intrusive_ptr<ExpressionContext> expCtx,
    const NamespaceString& nss,
    BSONObjBuilder& bob) {
    auto rawFilter = BSON(generateFieldName() << BSON("$eq" << 1));
    bob.appendElements(BSON("find" << nss.coll().toString() << "$db"
                                   << nss.dbName().serializeWithoutTenantPrefix_UNSAFE() << "filter"
                                   << rawFilter));
    auto findCmd = query_request_helper::makeFromFindCommand(std::move(bob.asTempObj()),
                                                             boost::none /* vts */,
                                                             nss.tenantId(),
                                                             kSerializationContext,
                                                             false /* apiStrict */);
    return uassertStatusOK(parsed_find_command::parse(expCtx, {std::move(findCmd)}));
}

// Generates a find command (which belongs to 'medium' query class) of the following form:
// {
//   find: <CollName>,
//   $db: <DbName>,
//   filter: {
//     $or: [
//       UUID(): 1,
//       UUID(): { $ne: 1},
//       UUID(): 2
//     ]
//   },
//   projection: {
//     _id: 0,
//     UUID(): 1,
//     UUID(): 1,
//   }
// }
std::unique_ptr<ParsedFindCommand> generateMediumParsedFindRequest(
    const boost::intrusive_ptr<ExpressionContext> expCtx,
    const NamespaceString& nss,
    BSONObjBuilder& bob) {
    auto fieldName = generateFieldName();
    auto rawFilter = BSON("$or" << BSON_ARRAY(BSON(generateFieldName() << 1)
                                              << BSON(generateFieldName() << BSON("$ne" << 1))
                                              << BSON(generateFieldName() << 2)));
    auto rawProjection = BSON("_id" << 0 << generateFieldName() << 1 << generateFieldName() << 1);
    bob.appendElements(BSON("find" << nss.coll().toString() << "$db"
                                   << nss.dbName().serializeWithoutTenantPrefix_UNSAFE() << "filter"
                                   << rawFilter << "projection" << rawProjection));
    auto findCmd = query_request_helper::makeFromFindCommand(std::move(bob.asTempObj()),
                                                             boost::none /* vts */,
                                                             nss.tenantId(),
                                                             kSerializationContext,
                                                             false /* apiStrict */);
    return uassertStatusOK(parsed_find_command::parse(expCtx, {std::move(findCmd)}));
}

// Generates a find command (which belongs to 'complex' query class) of the following form:
// {
//   find: <CollName>,
//   $db: <DbName>,
//   filter: {
//     $and: [
//       $or: {
//         UUID(): 1,
//         UUID(): { $ne: 1},
//         UUID(): 2
//       },
//       UUID(): {
//         $gte: rand(),
//         $lte: rand(),
//       },
//       UUID(): false,
//       UUID(): {
//         $nin: [rand(), rand(), rand()]
//       }
//     ]
//   },
//   projection: {
//     _id: 0,
//     UUID(): 1,
//     UUID(): [1, 2, 3, $UUID()],
//     total: {
//       $sum: UUID()
//     }
//   },
//   sort: {
//     _id: 1,
//     UUID(): -1
//   }
// }
std::unique_ptr<ParsedFindCommand> generateLargeParsedFindRequest(
    const boost::intrusive_ptr<ExpressionContext> expCtx,
    const NamespaceString& nss,
    BSONObjBuilder& bob) {
    PseudoRandom randomNumberGenerator(1 /* seed */);
    auto rawFilter =
        BSON("$or" << BSON_ARRAY(BSON(generateFieldName() << 1)
                                 << BSON(generateFieldName() << BSON("$ne" << 1))
                                 << BSON(generateFieldName() << 2))
                   << generateFieldName()
                   << BSON("$gte" << randomNumberGenerator.nextInt64() << "$lt"
                                  << randomNumberGenerator.nextInt64())
                   << generateFieldName() << false << generateFieldName()
                   << BSON("$nin" << BSON_ARRAY(randomNumberGenerator.nextInt64()
                                                << randomNumberGenerator.nextInt64()
                                                << randomNumberGenerator.nextInt64())));
    auto rawProjection = BSON("_id" << 0 << generateFieldName() << 1 << generateFieldName()
                                    << BSON_ARRAY(1 << 2 << 3 << "$field") << "total"
                                    << BSON("$sum" << generateFieldName()));
    auto rawSort = BSON("_id" << 1 << generateFieldName() << -1);
    bob.appendElements(BSON("find" << nss.coll().toString() << "$db"
                                   << nss.dbName().serializeWithoutTenantPrefix_UNSAFE() << "filter"
                                   << rawFilter << "projection" << rawProjection << "sort"
                                   << rawSort));
    auto findCmd = query_request_helper::makeFromFindCommand(std::move(bob.asTempObj()),
                                                             boost::none /* vts */,
                                                             nss.tenantId(),
                                                             kSerializationContext,
                                                             false /* apiStrict */);
    return uassertStatusOK(parsed_find_command::parse(expCtx, {std::move(findCmd)}));
}

/**
 * Generates command of the given 'complexity' and writes the command BSONObj into 'bob'.
 */
std::unique_ptr<ParsedFindCommand> generateParsedFindRequest(
    QueryClass queryClass,
    const boost::intrusive_ptr<ExpressionContext> expCtx,
    const NamespaceString& nss,
    BSONObjBuilder& bob) {
    switch (queryClass) {
        case QueryClass::kSmall:
            return generateSmallParsedFindRequest(expCtx, nss, bob);
        case QueryClass::kMedium:
            return generateMediumParsedFindRequest(expCtx, nss, bob);
        case QueryClass::kLarge:
            return generateLargeParsedFindRequest(expCtx, nss, bob);
        default:
            MONGO_UNREACHABLE;
    }
}
};  // namespace request_generator

class QuerySettingsLookupBenchmark : public benchmark::Fixture {
public:
    QuerySettingsLookupBenchmark() {}

    void TearDown(benchmark::State& state) override {
        stdx::lock_guard lk(_setupMutex);
        if (--_configuredThreads) {
            return;
        }

        setGlobalServiceContext({});
    }

    // Populates the query settings manager with dummy query shape configurations.
    void populateQueryShapeConfigurations(benchmark::State& state,
                                          const boost::optional<TenantId>& tenantId,
                                          int dummyQuerySettingsCount) {
        auto client = getGlobalServiceContext()->getService()->makeClient("setup");
        auto opCtx = client->makeOperationContext();
        auto expCtx =
            make_intrusive<ExpressionContext>(opCtx.get(), nullptr, makeNamespace(tenantId));
        std::vector<QueryShapeConfiguration> queryShapeConfigs;

        auto generateQueryShapeConfiguration =
            [&](const NamespaceString& nss,
                const std::string& fieldName) -> QueryShapeConfiguration {
            BSONObjBuilder bob;
            auto parsedFind = request_generator::generateSmallParsedFindRequest(expCtx, nss, bob);
            query_shape::FindCmdShape findCmdShape(*parsedFind, expCtx);
            auto query = bob.obj();

            QuerySettings querySettings;
            querySettings.setQueryFramework(QueryFrameworkControlEnum::kTrySbeEngine);
            return {
                findCmdShape.sha256Hash(opCtx.get(), kSerializationContext), querySettings, query};
        };

        for (auto i = 0; i < dummyQuerySettingsCount; i++) {
            queryShapeConfigs.push_back(
                generateQueryShapeConfiguration(makeNamespace(tenantId), generateFieldName()));
        }

        // Set the custom counter of the settings total size.
        auto querySettingsByteSize = dummyQuerySettingsCount *
            (queryShapeConfigs.empty() ? 0 : queryShapeConfigs[0].toBSON().objsize());
        state.counters["QuerySettingsByteSize"] =
            benchmark::Counter(querySettingsByteSize,
                               benchmark::Counter::Flags::kDefaults,
                               benchmark::Counter::OneK::kIs1024);

        // Update the QuerySettingsManager with the 'queryShapeConfigs'.
        auto& manager = QuerySettingsManager::get(opCtx.get());
        manager.setQueryShapeConfigurations(
            opCtx.get(), std::move(queryShapeConfigs), LogicalTime(Timestamp(1)), tenantId);
        benchmark::ClobberMemory();
    }

    boost::optional<TenantId> tenantId(int threadId) const {
        if (!isMultitenacyEnabled()) {
            return boost::none;
        }
        return TenantId(OID::fromTerm(threadId));
    }

    void runBenchmark(benchmark::State& state) {
        auto client = getGlobalServiceContext()->getService()->makeClient(
            str::stream() << "thread: " << state.thread_index);
        auto opCtx = client->makeOperationContext();
        auto tid = tenantId(state.thread_index);
        auto ns = makeNamespace(tid);
        auto expCtx = make_intrusive<ExpressionContext>(opCtx.get(), nullptr, ns);

        bool isTestingHitCase = state.range(0) > 0 && state.range(2) == 1;
        BSONObjBuilder bob;
        auto queryClass = static_cast<request_generator::QueryClass>(state.range(1));
        auto parsedFind = [&]() {
            auto parsedFindRequest =
                request_generator::generateParsedFindRequest(queryClass, expCtx, ns, bob);

            if (isTestingHitCase) {
                // Create new query shape configuration with the generated request.
                QuerySettings querySettings;
                querySettings.setQueryFramework(QueryFrameworkControlEnum::kTrySbeEngine);
                QueryShapeConfiguration hitQueryShapeConfiguration = {
                    query_shape::FindCmdShape(*parsedFindRequest, expCtx)
                        .sha256Hash(opCtx.get(), kSerializationContext),
                    querySettings,
                    bob.asTempObj().getOwned()};

                // Update the query shape configurations by adding a new one, which will be used for
                // the lookup.
                auto& manager = QuerySettingsManager::get(opCtx.get());
                auto queryShapeConfigurations =
                    manager.getAllQueryShapeConfigurations(opCtx.get(), tid);
                queryShapeConfigurations.push_back(hitQueryShapeConfiguration);
                manager.setQueryShapeConfigurations(opCtx.get(),
                                                    std::move(queryShapeConfigurations),
                                                    LogicalTime(Timestamp(2)),
                                                    tid);
            }

            return parsedFindRequest;
        }();
        auto querySize = parsedFind->findCommandRequest->toBSON(BSONObj()).objsize();

        state.SetLabel(str::stream()
                       << "QuerySettingsCount=" << state.range(0) << " QuerySizeBytes=" << querySize
                       << " QueryClass=" << queryClassToString(queryClass) << " Threads="
                       << state.threads << " HitOrMiss=" << (isTestingHitCase ? "Hit" : "Miss"));

        while (state.KeepRunning()) {
            benchmark::DoNotOptimize(
                query_settings::lookupQuerySettingsForFind(expCtx, *parsedFind, ns));
        }
    }

    virtual bool isMultitenacyEnabled() const = 0;

protected:
    Mutex _setupMutex;

    // Indicates how many threads have executed the setup code.
    size_t _configuredThreads = 0;

    boost::optional<RAIIServerParameterControllerForTest> _querySettingsFeatureFlag;
    boost::optional<RAIIServerParameterControllerForTest> _multitenancyFeatureFlag;
};

class QuerySettingsNotMultitenantLookupBenchmark : public QuerySettingsLookupBenchmark {
    void SetUp(benchmark::State& state) override {
        stdx::lock_guard lk(_setupMutex);
        if (!_configuredThreads++) {
            // Setup FCV.
            // (Generic FCV reference): required for enabling the feature flag.
            serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);

            // On the first launch, initialize global service context and the QuerySettingsManager.
            setGlobalServiceContext(ServiceContext::make());
            QuerySettingsManager::create(getGlobalServiceContext(), {});

            // Initialize the feature flag.
            _querySettingsFeatureFlag.emplace("featureFlagQuerySettings", true);

            // Query settings are populated only for a single tenant (global scope).
            auto numberOfExistingSettings = state.range(0);
            populateQueryShapeConfigurations(
                state, boost::none /* tenantId */, numberOfExistingSettings);
        }
    }

    bool isMultitenacyEnabled() const override {
        return false;
    }
};

class QuerySettingsMultiTenantLookupBenchmark : public QuerySettingsLookupBenchmark {
    void SetUp(benchmark::State& state) override {
        stdx::lock_guard lk(_setupMutex);
        if (!_configuredThreads++) {
            // Setup FCV.
            // (Generic FCV reference): required for enabling the feature flag.
            serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);

            // On the first launched thread, initialize global service context and the
            // QuerySettingsManager.
            setGlobalServiceContext(ServiceContext::make());
            QuerySettingsManager::create(getGlobalServiceContext(), {});

            // Initialize the feature flags.
            _querySettingsFeatureFlag.emplace("featureFlagQuerySettings", true);
            _multitenancyFeatureFlag.emplace("multitenancySupport", true);
        }

        // Query settings are populated for every tenant.
        auto numberOfExistingSettings = state.range(0);
        populateQueryShapeConfigurations(
            state, tenantId(state.thread_index), numberOfExistingSettings);
    }

    bool isMultitenacyEnabled() const override {
        return true;
    }
};

BENCHMARK_DEFINE_F(QuerySettingsNotMultitenantLookupBenchmark, BM_QuerySettingsLookup)
(benchmark::State& state) {
    // Run benchmark in non multi-tenant setup.
    runBenchmark(state);
}

BENCHMARK_DEFINE_F(QuerySettingsMultiTenantLookupBenchmark, BM_QuerySettingsLookup)
(benchmark::State& state) {
    // Run the benchmark in multi-tenant setup. Each thread will act as a separate tenant.
    runBenchmark(state);
}

/**
 * Adds arguments to the benchmark. We want to run the benchmark with the following number of query
 * settings:
 * - No query settings are set
 * - A single query setting is set in order to measure the impact of QueryShapeHash computation on
 * the lookup
 * - Maximum amount of query settings per tenant: 16MB, which equals around 75_500 query settings
 *
 * The lookup will be performed for small, medium and large queries.
 * The lookup performance will be tested for both miss and hit cases.
 * Query settings lookup benchmark will run on a single thread as well as on multiple threads.
 *
 * The reasoning behind various configurations is to understand the query settings lookup cost and
 * how does it differ when running in different setups.
 */
#define ADD_ARGS()                                                          \
    ArgsProduct({{0, 1, 75500},                                             \
                 {static_cast<int>(request_generator::QueryClass::kSmall),  \
                  static_cast<int>(request_generator::QueryClass::kMedium), \
                  static_cast<int>(request_generator::QueryClass::kLarge)}, \
                 {0, 1}})                                                   \
        ->ThreadRange(1, ProcessInfo::getNumAvailableCores())

BENCHMARK_REGISTER_F(QuerySettingsNotMultitenantLookupBenchmark, BM_QuerySettingsLookup)
    ->ADD_ARGS();
BENCHMARK_REGISTER_F(QuerySettingsMultiTenantLookupBenchmark, BM_QuerySettingsLookup)->ADD_ARGS();
}  // namespace
}  // namespace mongo::query_settings
