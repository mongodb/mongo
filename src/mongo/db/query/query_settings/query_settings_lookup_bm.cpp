// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/query/query_shape/find_cmd_shape.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/processinfo.h"

#include <benchmark/benchmark.h>

namespace mongo::query_settings {
namespace {

static auto const kSerializationContext = SerializationContext{
    SerializationContext::Source::Command, SerializationContext::CallerType::Request};

NamespaceString makeNamespace() {
    auto dbName = DatabaseName::createDatabaseName_forTest(/* tenantId */ boost::none, "db");
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
    bob.appendElements(BSON("find" << nss.coll() << "$db"
                                   << nss.dbName().serializeWithoutTenantPrefix_UNSAFE() << "filter"
                                   << rawFilter));
    auto findCmd = query_request_helper::makeFromFindCommand(std::move(bob.asTempObj()),
                                                             boost::none /* vts */,
                                                             /* tenantId */ boost::none,
                                                             kSerializationContext);
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
    bob.appendElements(BSON("find" << nss.coll() << "$db"
                                   << nss.dbName().serializeWithoutTenantPrefix_UNSAFE() << "filter"
                                   << rawFilter << "projection" << rawProjection));
    auto findCmd = query_request_helper::makeFromFindCommand(std::move(bob.asTempObj()),
                                                             boost::none /* vts */,
                                                             /* tenantId */ boost::none,
                                                             kSerializationContext);
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
    bob.appendElements(BSON(
        "find" << nss.coll() << "$db" << nss.dbName().serializeWithoutTenantPrefix_UNSAFE()
               << "filter" << rawFilter << "projection" << rawProjection << "sort" << rawSort));
    auto findCmd = query_request_helper::makeFromFindCommand(std::move(bob.asTempObj()),
                                                             boost::none /* vts */,
                                                             /* tenantId */ boost::none,
                                                             kSerializationContext);
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

    void SetUp(benchmark::State& state) override {
        std::lock_guard lk(_setupMutex);
        if (!_configuredThreads++) {
            // Setup FCV.
            // (Generic FCV reference): required for enabling the feature flag.
            serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);

            // On the first launch, initialize global service context and initialize the query
            // settings.
            setGlobalServiceContext(ServiceContext::make());
            query_settings::QuerySettingsService::initializeForTest(getGlobalServiceContext());
            _internalQuerySettingsDisableBackfillFlag.emplace(
                "internalQuerySettingsDisableBackfill", true);

            auto numberOfExistingSettings = state.range(0);
            populateQueryShapeConfigurations(state, numberOfExistingSettings);
        }
    }

    void TearDown(benchmark::State& state) override {
        std::lock_guard lk(_setupMutex);
        if (--_configuredThreads) {
            return;
        }

        setGlobalServiceContext({});
    }

    // Populates the system with dummy query shape configurations.
    void populateQueryShapeConfigurations(benchmark::State& state, int dummyQuerySettingsCount) {
        auto client = getGlobalServiceContext()->getService()->makeClient("setup");
        auto opCtx = client->makeOperationContext();
        auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx.get()).ns(makeNamespace()).build();
        std::vector<QueryShapeConfiguration> queryShapeConfigs;

        auto generateQueryShapeConfiguration =
            [&](const NamespaceString& nss,
                const std::string& fieldName) -> QueryShapeConfiguration {
            BSONObjBuilder bob;
            auto parsedFind = request_generator::generateSmallParsedFindRequest(expCtx, nss, bob);
            query_shape::FindCmdShape findCmdShape(*parsedFind, expCtx);

            QuerySettings querySettings;
            querySettings.setQueryFramework(QueryFrameworkControlEnum::kTrySbeEngine);
            return QueryShapeConfiguration(
                findCmdShape.sha256Hash(opCtx.get(), kSerializationContext), querySettings);
        };

        for (auto i = 0; i < dummyQuerySettingsCount; i++) {
            queryShapeConfigs.push_back(
                generateQueryShapeConfiguration(makeNamespace(), generateFieldName()));
        }

        // Set the custom counter of the settings total size.
        auto querySettingsByteSize = dummyQuerySettingsCount *
            (queryShapeConfigs.empty() ? 0 : queryShapeConfigs[0].toBSON().objsize());
        state.counters["QuerySettingsByteSize"] =
            benchmark::Counter(querySettingsByteSize,
                               benchmark::Counter::Flags::kDefaults,
                               benchmark::Counter::OneK::kIs1024);

        // Update the qurey shape configurations present in the system.
        QuerySettingsService::get(opCtx.get())
            .setAllQueryShapeConfigurations(QueryShapeConfigurationsWithTimestamp{
                std::move(queryShapeConfigs), LogicalTime(Timestamp(1))});
        benchmark::ClobberMemory();
    }

    void runBenchmark(benchmark::State& state) {
        auto client = getGlobalServiceContext()->getService()->makeClient(
            str::stream() << "thread: " << state.thread_index);
        auto opCtx = client->makeOperationContext();
        auto ns = makeNamespace();
        auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx.get()).ns(ns).build();

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
                QueryShapeConfiguration hitQueryShapeConfiguration{
                    query_shape::FindCmdShape(*parsedFindRequest, expCtx)
                        .sha256Hash(opCtx.get(), kSerializationContext),
                    querySettings};

                // Update the query shape configurations by adding a new one, which will be used for
                // the lookup.
                auto queryShapeConfigurationsWithTimestamp =
                    query_settings::QuerySettingsService::get(opCtx.get())
                        .getAllQueryShapeConfigurations();
                auto&& queryShapeConfigurations =
                    queryShapeConfigurationsWithTimestamp.queryShapeConfigurations;
                queryShapeConfigurations.push_back(hitQueryShapeConfiguration);

                QuerySettingsService::get(opCtx.get())
                    .setAllQueryShapeConfigurations(QueryShapeConfigurationsWithTimestamp{
                        std::move(queryShapeConfigurations), LogicalTime(Timestamp(2))});
            }

            return parsedFindRequest;
        }();
        auto querySize = parsedFind->findCommandRequest->toBSON().objsize();

        state.SetLabel(str::stream()
                       << "QuerySettingsCount=" << state.range(0) << " QuerySizeBytes=" << querySize
                       << " QueryClass=" << queryClassToString(queryClass) << " Threads="
                       << state.threads << " HitOrMiss=" << (isTestingHitCase ? "Hit" : "Miss"));

        auto& querySettingsService = query_settings::QuerySettingsService::get(opCtx.get());
        while (state.KeepRunning()) {
            query_shape::DeferredQueryShape deferredShape{[&]() {
                return shape_helpers::tryMakeShape<query_shape::FindCmdShape>(*parsedFind, expCtx);
            }};
            auto queryShapeHash =
                deferredShape().getValue()->sha256Hash(opCtx.get(), kSerializationContext);
            benchmark::DoNotOptimize(querySettingsService.lookupQuerySettingsWithRejectionCheck(
                expCtx, queryShapeHash, ns, boost::none));
        }
    }

protected:
    std::mutex _setupMutex;

    // Indicates how many threads have executed the setup code.
    size_t _configuredThreads = 0;

    boost::optional<unittest::ServerParameterGuard> _querySettingsFeatureFlag;
    boost::optional<unittest::ServerParameterGuard> _internalQuerySettingsDisableBackfillFlag;
};

BENCHMARK_DEFINE_F(QuerySettingsLookupBenchmark, BM_QuerySettingsLookup)
(benchmark::State& state) {
    runBenchmark(state);
}

/**
 * Adds arguments to the benchmark. We want to run the benchmark with the following number of query
 * settings:
 * - No query settings are set
 * - A single query setting is set in order to measure the impact of QueryShapeHash computation on
 * the lookup
 * - Maximum amount of query settings: 16MB, which equals around 75_500 query settings
 *
 * The lookup will be performed for small, medium and large queries.
 * The lookup performance will be tested for both miss and hit cases.
 * Query settings lookup benchmark will run on a single thread as well as on multiple threads.
 *
 * The reasoning behind various configurations is to understand the query settings lookup cost and
 * how it changes with query shape and concurrency.
 */
#define ADD_ARGS()                                                          \
    ArgsProduct({{0, 1, 75500},                                             \
                 {static_cast<int>(request_generator::QueryClass::kSmall),  \
                  static_cast<int>(request_generator::QueryClass::kMedium), \
                  static_cast<int>(request_generator::QueryClass::kLarge)}, \
                 {0, 1}})                                                   \
        ->ThreadRange(1, ProcessInfo::getNumAvailableCores())

BENCHMARK_REGISTER_F(QuerySettingsLookupBenchmark, BM_QuerySettingsLookup)->ADD_ARGS();
}  // namespace
}  // namespace mongo::query_settings
