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

#include "mongo/db/query/query_settings/query_settings_service.h"

#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/index_key_validate.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_settings/query_settings_backfill.h"
#include "mongo/db/query/query_settings/query_settings_cluster_parameter_gen.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_manager.h"
#include "mongo/db/query/query_settings/query_settings_service_dependencies.h"
#include "mongo/db/query/query_shape/agg_cmd_shape.h"
#include "mongo/db/query/query_shape/distinct_cmd_shape.h"
#include "mongo/db/query/query_shape/find_cmd_shape.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_cmds_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/serialization_context.h"

#include <string_view>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::query_settings {
using namespace std::literals::string_view_literals;
MONGO_FAIL_POINT_DEFINE(throwConflictingOperationInProgressOnQuerySettingsSetClusterParameter);

using namespace query_shape;

namespace {
// Explicitly defines the `SerializationContext` to be used in `RepresentativeQueryInfo` factory
// methods. This was done as part of SERVER-79909 to ensure that inner query commands correctly
// infer the `tenantId`.
const auto kSerializationContext =
    SerializationContext{SerializationContext::Source::Command,
                         SerializationContext::CallerType::Request,
                         SerializationContext::Prefix::ExcludePrefix};

const stdx::unordered_set<std::string_view, StringMapHasher> rejectionIncompatibleStages = {
    "$querySettings"sv,
    "$planCacheStats"sv,
    "$collStats"sv,
    "$indexStats"sv,
    "$listSessions"sv,
    "$listSampledQueries"sv,
    "$queryStats"sv,
    "$currentOp"sv,
    "$listCatalog"sv,
    "$listLocalSessions"sv,
    "$listSearchIndexes"sv,
};

const auto getQuerySettingsService =
    ServiceContext::declareDecoration<std::unique_ptr<QuerySettingsService>>();

static constexpr auto kQuerySettingsClusterParameterName = "querySettings"sv;

MONGO_FAIL_POINT_DEFINE(allowAllSetQuerySettings);

/**
 * If the pipeline starts with a "system"/administrative document source to which query settings
 * should not be applied, return the relevant stage name.
 */
boost::optional<std::string> getStageExemptedFromRejection(const std::vector<BSONObj>& pipeline) {
    if (pipeline.empty()) {
        return boost::none;
    }

    if (canPipelineBeRejected(pipeline)) {
        // No pipeline stages are incompatible with rejection.
        return boost::none;
    }

    // Currently, all "system" queries are always the first stage in a pipeline.
    std::string firstStageName{pipeline.at(0).firstElementFieldName()};
    return {std::move(firstStageName)};
}

void failIfRejectedBySettings(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              const QuerySettings& settings) {
    if (expCtx->getExplain() || !expCtx->canBeRejected()) {
        // Explaining queries which _would_ be rejected if executed is still useful;
        // do not fail here.
        return;
    }

    if (settings.getReject()) {
        auto* opCtx = expCtx->getOperationContext();
        auto* curOp = CurOp::get(opCtx);

        auto query = curOp->opDescription();
        mutablebson::Document cmdToLog(query, mutablebson::Document::kInPlaceDisabled);
        if (auto cmdInvocation = CommandInvocation::get(opCtx)) {
            cmdInvocation->definition()->snipForLogging(&cmdToLog);
        }

        LOGV2_DEBUG_OPTIONS(8687100,
                            2,
                            {logv2::LogComponent::kQueryRejected},
                            "Query rejected by QuerySettings",
                            "queryShapeHash"_attr =
                                curOp->debug().getQueryShapeHash()->toHexString(),
                            "ns"_attr = curOp->getNS(),
                            "command"_attr = redact(cmdToLog.getObject()));
        uasserted(ErrorCodes::QueryRejectedBySettings, "Query rejected by admin query settings");
    }
}

/*
 * Creates the corresponding RepresentativeQueryInfo for Find query representatives.
 */
RepresentativeQueryInfo createRepresentativeInfoFind(OperationContext* opCtx,
                                                     const QueryInstance& queryInstance,
                                                     const boost::optional<TenantId>& tenantId) {
    auto findCommandRequest = std::make_unique<FindCommandRequest>(
        FindCommandRequest::parse(queryInstance,
                                  IDLParserContext("findCommandRequest",
                                                   auth::ValidatedTenancyScope::get(opCtx),
                                                   tenantId,
                                                   kSerializationContext)));

    // Add the '$recordId' meta-projection field if needed. The 'addShowRecordIdMetaProj()' helper
    // function modifies the request in-place, therefore affecting the query shape.
    if (findCommandRequest->getShowRecordId()) {
        query_request_helper::addShowRecordIdMetaProj(findCommandRequest.get());
    }

    // Extract namespace from find command.
    auto& nssOrUuid = findCommandRequest->getNamespaceOrUUID();
    uassert(7746605,
            "Collection namespace string must be provided for setQuerySettings command",
            nssOrUuid.isNamespaceString());

    auto expCtx = makeBlankExpressionContext(opCtx, nssOrUuid.nss(), findCommandRequest->getLet());
    auto parsedFindCommand = uassertStatusOK(parsed_find_command::parse(
        expCtx,
        ParsedFindCommandParams{.findCommand = std::move(findCommandRequest),
                                .allowedFeatures =
                                    MatchExpressionParser::kAllowAllSpecialFeatures}));

    const auto serializationContext =
        parsedFindCommand->findCommandRequest->getSerializationContext();
    FindCmdShape findCmdShape{*parsedFindCommand, expCtx};
    auto serializedQueryShape =
        findCmdShape.toBson(opCtx,
                            query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions,
                            serializationContext);

    return RepresentativeQueryInfo{
        .serializedQueryShape = serializedQueryShape,
        .queryShapeHash = findCmdShape.sha256Hash(opCtx, serializationContext),
        .namespaceString = nssOrUuid.nss(),
        .involvedNamespaces = {nssOrUuid.nss()},
        .encryptionInformation = parsedFindCommand->findCommandRequest->getEncryptionInformation(),
        .isIdHackQuery =
            isIdHackEligibleQueryWithoutCollator(*parsedFindCommand->findCommandRequest),
        .systemStage = {/* no unsupported agg stages */},
        .isRawDataQuery = parsedFindCommand->findCommandRequest->getRawData().value_or(false),
    };
}

/*
 * Creates the corresponding RepresentativeQueryInfo for Distinct query representatives.
 */
RepresentativeQueryInfo createRepresentativeInfoDistinct(
    OperationContext* opCtx,
    const QueryInstance& queryInstance,
    const boost::optional<TenantId>& tenantId) {
    auto distinctCommandRequest = std::make_unique<DistinctCommandRequest>(
        DistinctCommandRequest::parse(queryInstance,
                                      IDLParserContext("distinctCommandRequest",
                                                       auth::ValidatedTenancyScope::get(opCtx),
                                                       tenantId,
                                                       kSerializationContext)));
    // Extract namespace from distinct command.
    auto& nssOrUuid = distinctCommandRequest->getNamespaceOrUUID();
    uassert(7919501,
            "Collection namespace string must be provided for setQuerySettings command",
            nssOrUuid.isNamespaceString());

    auto expCtx = makeBlankExpressionContext(opCtx, nssOrUuid);
    auto parsedDistinctCommand =
        parsed_distinct_command::parse(expCtx,
                                       std::move(distinctCommandRequest),
                                       ExtensionsCallbackNoop(),
                                       MatchExpressionParser::kAllowAllSpecialFeatures);


    DistinctCmdShape distinctCmdShape{*parsedDistinctCommand, expCtx};
    const auto serializationContext =
        parsedDistinctCommand->distinctCommandRequest->getSerializationContext();
    auto serializedQueryShape =
        distinctCmdShape.toBson(expCtx->getOperationContext(),
                                query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions,
                                serializationContext);

    return RepresentativeQueryInfo{
        .serializedQueryShape = serializedQueryShape,
        .queryShapeHash =
            distinctCmdShape.sha256Hash(expCtx->getOperationContext(), serializationContext),
        .namespaceString = nssOrUuid.nss(),
        .involvedNamespaces = {nssOrUuid.nss()},
        .encryptionInformation = boost::none,
        .isIdHackQuery = false,
        .systemStage = {/* no unsupported agg stages */},
        .isRawDataQuery =
            parsedDistinctCommand->distinctCommandRequest->getRawData().value_or(false),
    };
}

/*
 * Creates the corresponding RepresentativeQueryInfo for Aggregation query representatives.
 */
RepresentativeQueryInfo createRepresentativeInfoAgg(OperationContext* opCtx,
                                                    const QueryInstance& queryInstance,
                                                    const boost::optional<TenantId>& tenantId) {
    auto aggregateCommandRequest =
        AggregateCommandRequest::parse(queryInstance,
                                       IDLParserContext("aggregateCommandRequest",
                                                        auth::ValidatedTenancyScope::get(opCtx),
                                                        tenantId,
                                                        kSerializationContext));
    // Populate foreign collection namespaces.
    auto parsedPipeline = LiteParsedPipeline(aggregateCommandRequest);
    auto involvedNamespaces = parsedPipeline.getInvolvedNamespaces();
    // We also need to add the main namespace because 'addResolvedNamespaces' only
    // adds the foreign collections.
    auto resolvedNs = ResolvedNamespace{aggregateCommandRequest.getNamespace(),
                                        aggregateCommandRequest.getPipeline()};
    involvedNamespaces.insert(resolvedNs.getResolvedNamespace());

    // When parsing the pipeline, we try to resolve the namespaces, which requires the resolved
    // namespaces to be present in the expression context.
    auto nss = aggregateCommandRequest.getNamespace();
    auto expCtx = makeBlankExpressionContext(opCtx, nss, aggregateCommandRequest.getLet());
    expCtx->addResolvedNamespaces(
        stdx::unordered_set<NamespaceString>{involvedNamespaces.begin(), involvedNamespaces.end()});

    auto pipeline = pipeline_factory::makePipeline(
        aggregateCommandRequest.getPipeline(), expCtx, pipeline_factory::kOptionsMinimal);

    const auto serializationContext = aggregateCommandRequest.getSerializationContext();
    AggCmdShape aggCmdShape{aggregateCommandRequest, nss, involvedNamespaces, *pipeline, expCtx};
    auto serializedQueryShape =
        aggCmdShape.toBson(expCtx->getOperationContext(),
                           query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions,
                           serializationContext);

    // For aggregate queries, the check for IDHACK should not be taken into account due to the
    // complexity of determining if a pipeline is eligible or not for IDHACK.
    return RepresentativeQueryInfo{
        .serializedQueryShape = serializedQueryShape,
        .queryShapeHash =
            aggCmdShape.sha256Hash(expCtx->getOperationContext(), serializationContext),
        .namespaceString = nss,
        .involvedNamespaces = std::move(involvedNamespaces),
        .encryptionInformation = aggregateCommandRequest.getEncryptionInformation(),
        .isIdHackQuery = false,
        .systemStage = getStageExemptedFromRejection(aggregateCommandRequest.getPipeline()),
        .isRawDataQuery = aggregateCommandRequest.getRawData().value_or(false),
    };
}

void validateIndexKeyPatternStructure(const IndexHint& hint) {
    if (auto&& keyPattern = hint.getIndexKeyPattern()) {
        uassert(9646000, "key pattern index can't be empty", keyPattern->nFields() > 0);
        auto status = index_key_validate::validateKeyPattern(
            *keyPattern, IndexDescriptor::getDefaultIndexVersion());
        uassert(
            9646001, status.withContext("invalid index key pattern hint").reason(), status.isOK());
    }
};

/**
 * Validates that QueryShapeConfiguration is not specified for queries with queryable encryption.
 */
void validateQuerySettingsEncryptionInformation(
    const RepresentativeQueryInfo& representativeQueryInfo) {
    uassert(7746600,
            "Queries with encryption information are not allowed on setQuerySettings commands",
            !representativeQueryInfo.encryptionInformation);

    bool containsFLE2StateCollection =
        std::any_of(representativeQueryInfo.involvedNamespaces.begin(),
                    representativeQueryInfo.involvedNamespaces.end(),
                    [](const NamespaceString& ns) { return ns.isFLE2StateCollection(); });

    uassert(7746601,
            "setQuerySettings command is not allowed on queryable encryption state collections",
            !containsFLE2StateCollection);
}

void validateQuerySettingsKeyPatternIndexHints(const IndexHintSpec& hintSpec) {
    const auto& allowedIndexes = hintSpec.getAllowedIndexes();
    std::for_each(allowedIndexes.begin(), allowedIndexes.end(), validateIndexKeyPatternStructure);
}

/**
 * Validates that no index hint applies to the same collection more than once.
 */
void validateQuerySettingsIndexHints(const auto& indexHints) {
    // If there are no index hints involved, no validation is required.
    if (!indexHints) {
        return;
    }

    stdx::unordered_map<NamespaceString, mongo::query_settings::IndexHintSpec>
        collectionsWithAppliedIndexHints;
    for (const auto& hint : *indexHints) {
        uassert(8727500,
                "invalid index hint: 'ns.db' field is missing",
                hint.getNs().getDb().has_value());
        uassert(8727501,
                "invalid index hint: 'ns.coll' field is missing",
                hint.getNs().getColl().has_value());
        validateQuerySettingsKeyPatternIndexHints(hint);
        auto nss = NamespaceStringUtil::deserialize(*hint.getNs().getDb(), *hint.getNs().getColl());
        auto [it, emplaced] = collectionsWithAppliedIndexHints.emplace(nss, hint);
        uassert(7746608,
                str::stream() << "Collection '"
                              << collectionsWithAppliedIndexHints[nss].toBSON().toString()
                              << "' has already index hints specified",
                emplaced);
    }
}

void sanitizeKeyPatternIndexHints(QueryShapeConfiguration& queryShapeItem) {
    const auto isInvalidIndexHint = [&](const IndexHint& hint) {
        try {
            validateIndexKeyPatternStructure(hint);
            return false;
        } catch (const DBException&) {
            LOGV2_WARNING(9646002,
                          "invalid key pattern index hint in "
                          "query settings",
                          "indexHint"_attr = hint.getIndexKeyPattern()->toString(),
                          "queryShapeShash"_attr =
                              queryShapeItem.getQueryShapeHash().toHexString());
            return true;
        }
    };
    if (auto&& hints = queryShapeItem.getSettings().getIndexHints()) {
        for (auto&& spec : *hints) {
            std::erase_if(spec.getAllowedIndexes(), isInvalidIndexHint);
        }
    }
}

/**
 * Runs the 'fn' and retries up to 'maxNumRetries' on ConflictingOperationInProgress exception.
 */
template <typename Fn>
auto conflictingOperationInProgressRetry(Fn&& fn, size_t maxNumRetries = 100) {
    size_t numAttempts = 0;
    while (true) {
        try {
            return fn();
        } catch (const ExceptionFor<ErrorCodes::ConflictingOperationInProgress>& ex) {
            if (numAttempts == maxNumRetries) {
                throw;
            }

            logAndBackoff(10445112,
                          logv2::LogComponent::kQuery,
                          logv2::LogSeverity::Info(),
                          numAttempts,
                          "Caught ConflictingOperationInProgress",
                          "reason"_attr = ex.reason());
            numAttempts++;
        }
    }
}

/**
 * Returns a SetClusterParameter request object created from 'config'.
 */
SetClusterParameter makeQuerySettingsClusterParameter(
    const QueryShapeConfigurationsWithTimestamp& config) {
    BSONObjBuilder bob;
    BSONArrayBuilder arrayBuilder(
        bob.subarrayStart(QuerySettingsClusterParameterValue::kSettingsArrayFieldName));
    for (const auto& item : config.queryShapeConfigurations) {
        arrayBuilder.append(item.toBSON());
    }
    uassertStatusOK(arrayBuilder.done().validateBSONObjSize().addContext(
        "Setting query settings cluster parameter failed"));
    SetClusterParameter request(
        BSON(QuerySettingsService::getQuerySettingsClusterParameterName() << bob.done()));
    request.setDbName(DatabaseName::kConfig);
    return request;
}

class QuerySettingsMigration {
public:
    // The set of operations a migration may perform, as a bitmask. A migration plan is an OR of
    // these flags; 'run()' executes the present operations in a fixed, safe order.
    enum class Op : uint8_t {
        kNone = 0,
        kRemoveQueryKnobs = 1 << 0,
        kCreateCollection = 1 << 1,
        kMoveQueriesToCollection = 1 << 2,
        kMoveQueriesToParameter = 1 << 3,
        kDropCollection = 1 << 4,
    };

    friend constexpr Op operator|(Op a, Op b) {
        return static_cast<Op>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
    }
    friend constexpr Op& operator|=(Op& a, Op b) {
        return a = a | b;
    }
    static constexpr bool contains(Op plan, Op op) {
        return (static_cast<uint8_t>(plan) & static_cast<uint8_t>(op)) != 0;
    }

    QuerySettingsMigration(const QuerySettingsService* service) : _service(service) {}

    void run(OperationContext* opCtx, Op plan) {
        // Create the collection up front, before the retry loop populates it. The create is
        // idempotent and unrelated to the cluster parameter conflict being retried below.
        if (contains(plan, Op::kCreateCollection)) {
            _service->createQueryShapeRepresentativeQueriesCollection(opCtx);
        }

        conflictingOperationInProgressRetry([&] {
            _dirty = false;
            _config = _service->getAllQueryShapeConfigurations(boost::none /* tenantId */);
            LOGV2_DEBUG(12826800,
                        2,
                        "Running query settings migration pass",
                        "numQueryShapeConfigurations"_attr =
                            _config.queryShapeConfigurations.size());
            if (contains(plan, Op::kRemoveQueryKnobs)) {
                removeQueryKnobs();
            }
            if (contains(plan, Op::kMoveQueriesToCollection)) {
                moveQueriesToCollection(opCtx);
            }
            if (contains(plan, Op::kMoveQueriesToParameter)) {
                moveQueriesToParameter(opCtx);
            }
            if (_dirty) {
                LOGV2_DEBUG(12826801,
                            2,
                            "Persisting migrated query settings cluster parameter",
                            "numQueryShapeConfigurations"_attr =
                                _config.queryShapeConfigurations.size());
                _service->setQuerySettingsClusterParameter(opCtx, _config);
            }
        });

        // Drop the dedicated collection only after the representative queries have been persisted
        // back to the cluster parameter, so a failed write cannot lose them.
        if (contains(plan, Op::kDropCollection)) {
            _service->dropQueryShapeRepresentativeQueriesCollection(opCtx);
        }
    }

private:
    void removeQueryKnobs() {
        auto& configs = _config.queryShapeConfigurations;
        configs.erase(std::remove_if(configs.begin(),
                                     configs.end(),
                                     [&](auto&& entry) -> bool {
                                         auto&& settings = entry.getSettings();
                                         if (auto&& knobs = settings.getQueryKnobs()) {
                                             _dirty = true;
                                             // TODO SERVER-129571: Scope down query knob downgrades
                                             // only to knobs below the minimum FCV
                                             settings.setQueryKnobs(boost::none);
                                             const bool becameDefault = isDefault(settings);
                                             LOGV2_DEBUG(
                                                 12826802,
                                                 3,
                                                 "Stripping query knobs from query settings",
                                                 "queryShapeHash"_attr =
                                                     entry.getQueryShapeHash().toHexString(),
                                                 "removedEntry"_attr = becameDefault);
                                             if (becameDefault) {
                                                 return true;
                                             }
                                             entry.setSettings(settings);
                                         }
                                         return false;
                                     }),
                      configs.end());
    }

    void moveQueriesToCollection(OperationContext* opCtx) {
        const auto& clusterParameterTime = _config.clusterParameterTime;
        std::vector<QueryShapeRepresentativeQuery> representativeQueries;
        for (auto&& shapeConfig : _config.queryShapeConfigurations) {
            auto&& representativeQuery = shapeConfig.getRepresentativeQuery();
            if (!representativeQuery) {
                continue;
            }
            representativeQueries.emplace_back(
                shapeConfig.getQueryShapeHash(), *representativeQuery, clusterParameterTime);
            // Clear the 'representativeQuery' information as it will be stored in a separate
            // collection.
            shapeConfig.setRepresentativeQuery(boost::none);
            _dirty = true;
        }
        if (!representativeQueries.empty()) {
            _service->upsertRepresentativeQueries(opCtx, representativeQueries);
        }
    }

    void moveQueriesToParameter(OperationContext* opCtx) {
        auto& configs = _config.queryShapeConfigurations;
        auto setRepresentativeQuery = [&](auto representativeQuery) -> bool {
            auto it = std::find_if(configs.begin(), configs.end(), [&](const auto& shapeConfig) {
                return shapeConfig.getQueryShapeHash() == representativeQuery.get_id();
            });
            if (it != configs.end()) {
                it->setRepresentativeQuery(representativeQuery.getRepresentativeQuery());
                return true;
            }
            return false;
        };

        // Migrate representative queries from smallest to largest, stopping once approaching
        // BSONObjMaxUserSize limit.
        DBDirectClient client(opCtx);
        auto cursor = client.find([] {
            FindCommandRequest request{NamespaceString::kQueryShapeRepresentativeQueriesNamespace};
            BSONObjBuilder projection;
            projection.append(QueryShapeRepresentativeQuery::k_idFieldName, 1);
            projection.append(QueryShapeRepresentativeQuery::kRepresentativeQueryFieldName, 1);
            projection.append(QueryShapeRepresentativeQuery::kLastModifiedTimeFieldName, 1);
            std::string dollarRepresentativeQuery = str::stream()
                << "$" << QueryShapeRepresentativeQuery::kRepresentativeQueryFieldName;
            projection.append("bsonSize", BSON("$bsonSize" << dollarRepresentativeQuery));
            request.setProjection(projection.obj().getOwned());

            // Prioritize by 'representativeQuery' size, so that we can migrate as many
            // representative queries as possible.
            // TODO SERVER-107307: Introduce additional representative query size limits test
            // coverage.
            request.setSort(BSON("bsonSize" << 1));
            return request;
        }());
        IDLParserContext ctx{"QueryShapeRepresentativeQuery"};
        int budget =
            BSONObjMaxUserSize - makeQuerySettingsClusterParameter(_config).toBSON().objsize();
        while (cursor->more()) {
            BSONObj doc = cursor->next();
            int cost =
                doc.getField(QueryShapeRepresentativeQuery::kRepresentativeQueryFieldName).size();
            if (cost > budget) {
                break;
            }
            if (setRepresentativeQuery(QueryShapeRepresentativeQuery::parse(doc, ctx))) {
                budget -= cost;
                _dirty = true;
            }
        }
        if (!cursor->isDead()) {
            cursor->kill();
        }
    }

    const QuerySettingsService* _service;
    QueryShapeConfigurationsWithTimestamp _config;
    bool _dirty = false;
};

}  // namespace

class QuerySettingsRouterService : public QuerySettingsService {
public:
    QuerySettingsRouterService() {
        _backfillCoordinator = BackfillCoordinator::create(
            /* onCompletionHook */ [this](std::vector<QueryShapeHash> hashes,
                                          LogicalTime clusterParameterTime,
                                          boost::optional<TenantId> tenantId) {
                _manager.markBackfilledRepresentativeQueries(
                    hashes, clusterParameterTime, tenantId);
            });
    }

    QueryShapeConfigurationsWithTimestamp getAllQueryShapeConfigurations(
        const boost::optional<TenantId>& tenantId) const final {
        return _manager.getAllQueryShapeConfigurations(tenantId);
    }

    void setAllQueryShapeConfigurations(QueryShapeConfigurationsWithTimestamp&& config,
                                        const boost::optional<TenantId>& tenantId) final {
        _manager.setAllQueryShapeConfigurations(std::move(config), tenantId);
    }

    void removeAllQueryShapeConfigurations(const boost::optional<TenantId>& tenantId) final {
        _manager.removeAllQueryShapeConfigurations(tenantId);
    }

    LogicalTime getClusterParameterTime(const boost::optional<TenantId>& tenantId) const final {
        return _manager.getClusterParameterTime(tenantId);
    }

    QuerySettings lookupQuerySettingsWithRejectionCheck(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const query_shape::QueryShapeHash& queryShapeHash,
        const NamespaceString& nss,
        const boost::optional<QuerySettings>& querySettingsFromOriginalCommand) const override {
        if (expCtx->isIdHackQuery()) {
            return QuerySettings();
        }

        // Always perform cluster lookup (includes rejection check for cluster PQS).
        auto settings = lookupQuerySettingsFromInternalStorage(expCtx, queryShapeHash, nss);
        if (querySettingsFromOriginalCommand.has_value()) {
            // Merge: user settings as base, cluster settings override on conflict.
            settings = mergeQuerySettings(*querySettingsFromOriginalCommand, settings);
        }
        return settings;
    }

    void setQuerySettingsClusterParameter(
        OperationContext* opCtx,
        const QueryShapeConfigurationsWithTimestamp& config,
        boost::optional<LogicalTime> newClusterParameterTime) const override {
        MONGO_UNREACHABLE_TASSERT(10397900);
    }

    void createQueryShapeRepresentativeQueriesCollection(OperationContext* opCtx) const override {
        MONGO_UNIMPLEMENTED_TASSERT(10445100);
    }

    void dropQueryShapeRepresentativeQueriesCollection(OperationContext* opCtx) const override {
        MONGO_UNIMPLEMENTED_TASSERT(10445101);
    }

    void upgradeQuerySettings(OperationContext* opCtx,
                              multiversion::FeatureCompatibilityVersion targetFCV) const override {
        MONGO_UNIMPLEMENTED_TASSERT(10445102);
    }

    void downgradeQuerySettings(
        OperationContext* opCtx,
        multiversion::FeatureCompatibilityVersion targetFCV) const override {
        MONGO_UNIMPLEMENTED_TASSERT(10445103);
    }

    void upsertRepresentativeQueries(
        OperationContext* opCtx,
        const std::vector<QueryShapeRepresentativeQuery>& representativeQueries) const override {
        MONGO_UNIMPLEMENTED_TASSERT(10445104);
    }

    void deleteQueryShapeRepresentativeQuery(
        OperationContext* opCtx,
        const query_shape::QueryShapeHash& queryShapeHash,
        LogicalTime latestClusterParameterTime) const override {
        MONGO_UNIMPLEMENTED_TASSERT(10445105);
    }

protected:
    /**
     * Looks up query settings from the internal cluster parameter storage and runs the rejection
     * check. Does NOT consider command-level settings.
     */
    QuerySettings lookupQuerySettingsFromInternalStorage(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const query_shape::QueryShapeHash& queryShapeHash,
        const NamespaceString& nss) const {
        QuerySettings settings = [&]() {
            try {
                if (!isEligbleForQuerySettings(expCtx, nss)) {
                    return QuerySettings();
                }

                // Return the found query settings or an empty one.
                auto result =
                    _manager.getQuerySettingsForQueryShapeHash(queryShapeHash, nss.tenantId());
                if (!result.has_value()) {
                    return QuerySettings();
                }

                // Backfill the representative query if needed.
                auto* opCtx = expCtx->getOperationContext();
                if (BackfillCoordinator::shouldBackfill(expCtx, result->hasRepresentativeQuery)) {
                    _backfillCoordinator->markForBackfillAndScheduleIfNeeded(
                        opCtx, queryShapeHash, CurOp::get(opCtx)->opDescription().getOwned());
                }
                return std::move(result->querySettings);
            } catch (const DBException& ex) {
                LOGV2_WARNING_OPTIONS(10153400,
                                      {logv2::LogComponent::kQuery},
                                      "Failed to perform query settings lookup",
                                      "error"_attr = ex.toString());
                return QuerySettings();
            }
        }();

        // Fail the current command, if 'reject: true' flag is present.
        failIfRejectedBySettings(expCtx, settings);

        return settings;
    }

private:
    QuerySettingsManager _manager;
    std::unique_ptr<BackfillCoordinator> _backfillCoordinator;
};

class QuerySettingsShardService : public QuerySettingsRouterService {
public:
    QuerySettingsShardService(SetClusterParameterFn setClusterParameterFn)
        : _setClusterParameterFn(std::move(setClusterParameterFn)) {}

    QuerySettings lookupQuerySettingsWithRejectionCheck(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const query_shape::QueryShapeHash& queryShapeHash,
        const NamespaceString& nss,
        const boost::optional<QuerySettings>& querySettingsFromOriginalCommand) const override {
        if (expCtx->isIdHackQuery()) {
            return QuerySettings();
        }

        auto* opCtx = expCtx->getOperationContext();
        if (isInternalOrDirectClient(opCtx->getClient())) {
            // Mongos already looked up and merged - return forwarded settings as-is.
            return querySettingsFromOriginalCommand.get_value_or(QuerySettings());
        }

        // Replica set: perform cluster lookup (includes rejection check) and merge with user
        // settings.
        auto settings = lookupQuerySettingsFromInternalStorage(expCtx, queryShapeHash, nss);
        if (querySettingsFromOriginalCommand.has_value()) {
            settings = mergeQuerySettings(*querySettingsFromOriginalCommand, settings);
        }
        return settings;
    }

    void setQuerySettingsClusterParameter(
        OperationContext* opCtx,
        const QueryShapeConfigurationsWithTimestamp& config,
        boost::optional<LogicalTime> newClusterParameterTime = boost::none) const final {
        try {
            if (MONGO_unlikely(throwConflictingOperationInProgressOnQuerySettingsSetClusterParameter
                                   .shouldFail())) {
                uasserted(ErrorCodes::ConflictingOperationInProgress,
                          "ConflictingOperationInProgress");
            }

            auto request = makeQuerySettingsClusterParameter(config);
            auto newClusterParameterTimeAsTs = newClusterParameterTime
                ? boost::optional<Timestamp>(newClusterParameterTime->asTimestamp())
                : boost::none;
            tassert(10445106, "setClusterParameter must be provided", _setClusterParameterFn);
            _setClusterParameterFn(
                opCtx, request, newClusterParameterTimeAsTs, config.clusterParameterTime);
        } catch (const ExceptionFor<ErrorCodes::BSONObjectTooLarge>&) {
            uasserted(ErrorCodes::BSONObjectTooLarge,
                      str::stream() << "cannot modify query settings: the total size exceeds "
                                    << BSONObjMaxInternalSize << " bytes");
        }
    }

    void createQueryShapeRepresentativeQueriesCollection(OperationContext* opCtx) const override {
        constexpr auto& nss = NamespaceString::kQueryShapeRepresentativeQueriesNamespace;
        DBDirectClient client(opCtx);
        std::ignore = client.createCollection(nss);
    }

    void dropQueryShapeRepresentativeQueriesCollection(OperationContext* opCtx) const override {
        constexpr auto& nss = NamespaceString::kQueryShapeRepresentativeQueriesNamespace;
        DBDirectClient client(opCtx);
        std::ignore = client.dropCollection(nss);
    }


    void upgradeQuerySettings(OperationContext* opCtx,
                              multiversion::FeatureCompatibilityVersion targetFCV) const override {
        using Op = QuerySettingsMigration::Op;

        // Move representative queries into the dedicated collection once backfill is enabled.
        auto plan = Op::kNone;
        if (feature_flags::gFeatureFlagPQSBackfill.isEnabledOnVersion(targetFCV)) {
            plan |= Op::kCreateCollection | Op::kMoveQueriesToCollection;
        }

        LOGV2_DEBUG(12826803,
                    2,
                    "Planning query settings FCV upgrade",
                    "targetFCV"_attr = multiversion::toString(targetFCV),
                    "migrateRepresentativeQueriesToCollection"_attr =
                        QuerySettingsMigration::contains(plan, Op::kMoveQueriesToCollection));
        if (plan != Op::kNone) {
            QuerySettingsMigration(this).run(opCtx, plan);
        }
    }

    void downgradeQuerySettings(
        OperationContext* opCtx,
        multiversion::FeatureCompatibilityVersion targetFCV) const override {
        using Op = QuerySettingsMigration::Op;

        auto plan = Op::kNone;
        // Move representative queries back to the cluster parameter once backfill is disabled.
        if (!feature_flags::gFeatureFlagPQSBackfill.isEnabledOnVersion(targetFCV)) {
            plan |= Op::kMoveQueriesToParameter | Op::kDropCollection;
        }
        // Strip query knobs once the target FCV no longer supports them.
        if (!feature_flags::gFeatureFlagPqsQueryKnobs.isEnabledOnVersion(targetFCV)) {
            plan |= Op::kRemoveQueryKnobs;
        }

        LOGV2_DEBUG(12826804,
                    2,
                    "Planning query settings FCV downgrade",
                    "targetFCV"_attr = multiversion::toString(targetFCV),
                    "removeQueryKnobs"_attr =
                        QuerySettingsMigration::contains(plan, Op::kRemoveQueryKnobs),
                    "migrateRepresentativeQueriesToClusterParameter"_attr =
                        QuerySettingsMigration::contains(plan, Op::kMoveQueriesToParameter));

        if (plan != Op::kNone) {
            QuerySettingsMigration(this).run(opCtx, plan);
        }
    }

    void upsertRepresentativeQueries(
        OperationContext* opCtx,
        const std::vector<QueryShapeRepresentativeQuery>& representativeQueries) const override {
        const auto& nss = NamespaceString::kQueryShapeRepresentativeQueriesNamespace;
        DBDirectClient client(opCtx);
        for (auto&& query : representativeQueries) {
            try {
                client.update(nss,
                              BSON("_id" << query.get_id().toHexString()),
                              query.toBSON(),
                              true /* upsert */,
                              false /* multi */,
                              defaultMajorityWriteConcern().toBSON());
            } catch (const DBException& ex) {
                LOGV2_DEBUG(10445110,
                            1,
                            "Error occurred when upserting the representative query",
                            "error"_attr = redact(ex));
            }
        }
    }

    void deleteQueryShapeRepresentativeQuery(
        OperationContext* opCtx,
        const query_shape::QueryShapeHash& queryShapeHash,
        LogicalTime latestClusterParameterTime) const override {
        const auto& nss = NamespaceString::kQueryShapeRepresentativeQueriesNamespace;
        DBDirectClient client(opCtx);
        try {
            BSONObj predicate =
                BSON("_id" << queryShapeHash.toHexString()
                           << QueryShapeRepresentativeQuery::kLastModifiedTimeFieldName
                           << BSON("$lte" << latestClusterParameterTime.asTimestamp()));
            client.remove(
                nss, predicate, false /* removeMany */, defaultMajorityWriteConcern().toBSON());
        } catch (const DBException& ex) {
            LOGV2_DEBUG(10445111,
                        1,
                        "Error occurred when deleting the representative query",
                        "error"_attr = redact(ex));
        }
    }

private:
    SetClusterParameterFn _setClusterParameterFn;
};

QuerySettingsService& QuerySettingsService::get(ServiceContext* service) {
    return *getQuerySettingsService(service);
}

QuerySettingsService& QuerySettingsService::get(OperationContext* opCtx) {
    return *getQuerySettingsService(opCtx->getServiceContext());
}

std::string QuerySettingsService::getQuerySettingsClusterParameterName() {
    return std::string{kQuerySettingsClusterParameterName};
}

bool QuerySettingsService::isEligbleForQuerySettings(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const NamespaceString& nss) {
    // Query settings can not be set for IDHACK queries.
    if (expCtx->isIdHackQuery()) {
        return false;
    }

    // Query settings can not be set for queries with encryption information.
    if (expCtx->isFleQuery()) {
        return false;
    }

    // Query settings can not be set on internal dbs or system collections in user dbs.
    if (nss.isOnInternalDb() || nss.isSystem()) {
        return false;
    }

    return true;
}

const stdx::unordered_set<std::string_view, StringMapHasher>&
QuerySettingsService::getRejectionIncompatibleStages() {
    return rejectionIncompatibleStages;
};

void QuerySettingsService::initializeForRouter(ServiceContext* serviceContext) {
    getQuerySettingsService(serviceContext) = std::make_unique<QuerySettingsRouterService>();
}

void QuerySettingsService::initializeForShard(ServiceContext* serviceContext,
                                              SetClusterParameterFn setClusterParameterFn) {
    getQuerySettingsService(serviceContext) =
        std::make_unique<QuerySettingsShardService>(std::move(setClusterParameterFn));
}

void QuerySettingsService::initializeForTest(ServiceContext* serviceContext) {
    initializeForShard(serviceContext, nullptr);
}

RepresentativeQueryInfo createRepresentativeInfo(OperationContext* opCtx,
                                                 const BSONObj& cmd,
                                                 const boost::optional<TenantId>& tenantId) {
    const auto commandName = cmd.firstElementFieldNameStringData();
    if (commandName == FindCommandRequest::kCommandName) {
        return createRepresentativeInfoFind(opCtx, cmd, tenantId);
    }
    if (commandName == AggregateCommandRequest::kCommandName) {
        return createRepresentativeInfoAgg(opCtx, cmd, tenantId);
    }
    if (commandName == DistinctCommandRequest::kCommandName) {
        return createRepresentativeInfoDistinct(opCtx, cmd, tenantId);
    }
    uasserted(7746402, str::stream() << "QueryShape can not be computed for command: " << cmd);
}

bool canPipelineBeRejected(const std::vector<BSONObj>& pipeline) {
    return pipeline.empty() ||
        !QuerySettingsService::getRejectionIncompatibleStages().contains(
            pipeline.at(0).firstElementFieldName());
}

bool allowQuerySettingsFromClient(Client* client) {
    // Query settings are allowed to be part of the request when:
    // - the request comes from router (internal client), which has already performed the query
    //   settings lookup, or
    // - has been created internally and is executed via DBDirectClient, or
    // - the featureFlagAllowUserFacingQuerySettings is enabled, allowing external clients to pass
    //   querySettings directly as runtime hints.
    return isInternalOrDirectClient(client) ||
        feature_flags::gFeatureFlagAllowUserFacingQuerySettings.isEnabled();
}

bool isDefault(const QuerySettings& settings) {
    // The 'serialization_context' and 'comment' fields are not significant.
    static_assert(QuerySettings::fieldNames.size() == 6,
                  "A new field has been added to the QuerySettings structure, isDefault should be "
                  "updated appropriately.");

    // For the 'reject' field of type OptionalBool, consider both 'false' and missing value as
    // default.
    return !(settings.getQueryFramework() || settings.getIndexHints() || settings.getReject() ||
             settings.getQueryKnobs());
}

QuerySettings mergeQuerySettings(const QuerySettings& lhs, const QuerySettings& rhs) {
    static_assert(
        QuerySettings::fieldNames.size() == 6,
        "A new field has been added to the QuerySettings structure, mergeQuerySettings() should be "
        "updated appropriately.");

    QuerySettings querySettings = lhs;

    if (rhs.getQueryFramework()) {
        querySettings.setQueryFramework(rhs.getQueryFramework());
    }

    if (rhs.getIndexHints()) {
        querySettings.setIndexHints(rhs.getIndexHints());
    }

    // Note: update if reject has a value in the rhs, not just if that value is true.
    if (rhs.getReject().has_value()) {
        querySettings.setReject(rhs.getReject());
    }

    if (auto comment = rhs.getComment()) {
        querySettings.setComment(comment);
    }

    if (rhs.getQueryKnobs()) {
        auto merged = QuerySettingsKnobOverrides::merge(
            querySettings.getQueryKnobs().value_or(QuerySettingsKnobOverrides{}),
            *rhs.getQueryKnobs());
        querySettings.setQueryKnobs(std::move(merged));
    }

    return querySettings;
}

void QuerySettingsService::validateQuerySettings(const QuerySettings& querySettings) const {
    // Validates that the settings field for query settings is not empty.
    uassert(7746604,
            "the resulting settings cannot be empty or contain only default values",
            !isDefault(querySettings));

    // Validates that no query knob is overridden more than once. Entries are sorted by id, so
    // duplicates are adjacent. Also tasserts that simplifyQuerySettings() was called first so that
    // no DeleteQueryKnobOverride sentinels survive into stored settings.
    if (const auto& knobs = querySettings.getQueryKnobs()) {
        auto entries = knobs->entries();
        for (size_t i = 0; i < entries.size(); ++i) {
            tassert(12366200,
                    "DeleteQueryKnobOverride must not survive past simplification",
                    !std::holds_alternative<DeleteQueryKnobOverride>(entries[i].value));
            uassert(12366201,
                    "query knob overrides cannot contain duplicate knobs",
                    i == 0 || entries[i].id != entries[i - 1].id);
        }
    }

    validateQuerySettingsIndexHints(querySettings.getIndexHints());
}

void QuerySettingsService::validateQueryCompatibleWithAnyQuerySettings(
    const RepresentativeQueryInfo& representativeQueryInfo) const {
    if (MONGO_unlikely(allowAllSetQuerySettings.shouldFail())) {
        return;
    }
    uassert(8584900,
            "setQuerySettings command cannot be used on internal databases",
            !representativeQueryInfo.namespaceString.isOnInternalDb());

    uassert(8584901,
            "setQuerySettings command cannot be used on system collections",
            !representativeQueryInfo.namespaceString.isSystem());

    uassert(1064380,
            "setQuerySetting command cannot be used with rawData enabled",
            !representativeQueryInfo.isRawDataQuery);

    validateQuerySettingsEncryptionInformation(representativeQueryInfo);

    // Validates that the query settings' representative is not eligible for IDHACK.
    uassert(7746606,
            "setQuerySettings command cannot be used on find queries eligible for IDHACK",
            !representativeQueryInfo.isIdHackQuery);
}

void QuerySettingsService::validateQueryCompatibleWithQuerySettings(
    const RepresentativeQueryInfo& representativeQueryInfo, const QuerySettings& settings) const {
    if (MONGO_unlikely(allowAllSetQuerySettings.shouldFail())) {
        return;
    }
    uassert(8705200,
            str::stream() << "Setting {reject:true} is forbidden for query containing stage: "
                          << *representativeQueryInfo.systemStage,
            !(settings.getReject() && representativeQueryInfo.systemStage.has_value()));
}

void QuerySettingsService::validateQueryShapeConfigurations(
    const QueryShapeConfigurationsWithTimestamp& config) const {
    std::ignore = makeQuerySettingsClusterParameter(config).toBSON();
}

void QuerySettingsService::simplifyQuerySettings(QuerySettings& settings) const {
    // If reject is present, but is false, set to an empty optional.
    if (settings.getReject().has_value() && !settings.getReject()) {
        settings.setReject({});
    }
    if (auto knobs = settings.getQueryKnobs()) {
        knobs->simplify();
        settings.setQueryKnobs(knobs->empty() ? boost::none : knobs);
    }

    const auto& indexes = settings.getIndexHints();
    if (!indexes) {
        return;
    }

    // Remove index hints where list of allowed indexes is empty.
    IndexHintSpecs simplifiedIndexHints;
    std::copy_if(indexes->begin(),
                 indexes->end(),
                 std::back_inserter(simplifiedIndexHints),
                 [](const auto& spec) { return !spec.getAllowedIndexes().empty(); });

    if (simplifiedIndexHints.empty()) {
        settings.setIndexHints(boost::none);
    } else {
        settings.setIndexHints({{std::move(simplifiedIndexHints)}});
    }
}

// TODO SERVER-97546 Remove PQS index hint sanitization.
void QuerySettingsService::sanitizeQuerySettingsHints(
    std::vector<QueryShapeConfiguration>& queryShapeConfigs) const {
    std::erase_if(queryShapeConfigs, [&](QueryShapeConfiguration& queryShapeItem) {
        auto& settings = queryShapeItem.getSettings();
        sanitizeKeyPatternIndexHints(queryShapeItem);
        simplifyQuerySettings(settings);
        if (isDefault(settings)) {
            LOGV2_WARNING(9646003,
                          "query settings became default after index hint sanitization",
                          "queryShapeShash"_attr = queryShapeItem.getQueryShapeHash().toHexString(),
                          "queryInstance"_attr = queryShapeItem.getRepresentativeQuery().map(
                              [](const BSONObj& b) { return redact(b); }));
            return true;
        }
        return false;
    });
}

namespace {
ServiceContext::ConstructorActionRegisterer querySettingsServiceRegisterer(
    "QuerySettingsService",
    {},
    [](ServiceContext* serviceContext) {
        invariant(serviceContext);
        auto& dependencies = getServiceDependencies(serviceContext);
        auto role = serverGlobalParams.clusterRole;
        if (role.hasExclusively(ClusterRole::RouterServer)) {
            QuerySettingsService::initializeForRouter(serviceContext);
        } else {
            QuerySettingsService::initializeForShard(serviceContext,
                                                     role.has(ClusterRole::ConfigServer)
                                                         ? dependencies.setClusterParameterConfigsvr
                                                         : dependencies.setClusterParameterReplSet);
        }
    },
    {});
}  // namespace
}  // namespace mongo::query_settings
