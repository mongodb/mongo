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

#include "mongo/db/cluster_parameters/cluster_server_parameter_cmds_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/index_key_validate.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/query/query_settings/query_settings_backfill.h"
#include "mongo/db/query/query_settings/query_settings_cluster_parameter_gen.h"
#include "mongo/db/query/query_settings/query_settings_manager.h"
#include "mongo/db/query/query_settings/query_settings_service_dependencies.h"
#include "mongo/db/query/query_shape/agg_cmd_shape.h"
#include "mongo/db/query/query_shape/distinct_cmd_shape.h"
#include "mongo/db/query/query_shape/find_cmd_shape.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/serialization_context.h"

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::query_settings {
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

const stdx::unordered_set<StringData, StringMapHasher> rejectionIncompatibleStages = {
    "$querySettings"_sd,
    "$planCacheStats"_sd,
    "$collStats"_sd,
    "$indexStats"_sd,
    "$listSessions"_sd,
    "$listSampledQueries"_sd,
    "$queryStats"_sd,
    "$currentOp"_sd,
    "$listCatalog"_sd,
    "$listLocalSessions"_sd,
    "$listSearchIndexes"_sd,
};

const auto getQuerySettingsService =
    ServiceContext::declareDecoration<std::unique_ptr<QuerySettingsService>>();

static constexpr auto kQuerySettingsClusterParameterName = "querySettings"_sd;

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
    auto serializedQueryShape = findCmdShape.toBson(
        opCtx, SerializationOptions::kDebugQueryShapeSerializeOptions, serializationContext);

    return RepresentativeQueryInfo{
        .serializedQueryShape = serializedQueryShape,
        .queryShapeHash = findCmdShape.sha256Hash(opCtx, serializationContext),
        .namespaceString = nssOrUuid.nss(),
        .involvedNamespaces = {nssOrUuid.nss()},
        .encryptionInformation = parsedFindCommand->findCommandRequest->getEncryptionInformation(),
        .isIdHackQuery =
            isIdHackEligibleQueryWithoutCollator(*parsedFindCommand->findCommandRequest),
        .systemStage = {/* no unsupported agg stages */},
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
                                SerializationOptions::kDebugQueryShapeSerializeOptions,
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
    involvedNamespaces.insert(resolvedNs.ns);

    // When parsing the pipeline, we try to resolve the namespaces, which requires the resolved
    // namespaces to be present in the expression context.
    auto nss = aggregateCommandRequest.getNamespace();
    auto expCtx = makeBlankExpressionContext(opCtx, nss, aggregateCommandRequest.getLet());
    expCtx->addResolvedNamespaces(
        stdx::unordered_set<NamespaceString>{involvedNamespaces.begin(), involvedNamespaces.end()});

    // In order to parse a change stream request, 'inRouter' needs to be set to true.
    if (parsedPipeline.hasChangeStream()) {
        expCtx->setInRouter(true);
    }

    auto pipeline = Pipeline::parse(aggregateCommandRequest.getPipeline(), expCtx);

    const auto serializationContext = aggregateCommandRequest.getSerializationContext();
    AggCmdShape aggCmdShape{aggregateCommandRequest, nss, involvedNamespaces, *pipeline, expCtx};
    auto serializedQueryShape =
        aggCmdShape.toBson(expCtx->getOperationContext(),
                           SerializationOptions::kDebugQueryShapeSerializeOptions,
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
    };
}

bool requestComesFromRouterOrSentDirectlyToShard(Client* client) {
    return client->isInternalClient() || client->isInDirectClient();
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
        const boost::optional<QuerySettings>& querySettingsFromOriginalCommand =
            boost::none) const override {
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

    void migrateRepresentativeQueriesFromQuerySettingsClusterParameterToDedicatedCollection(
        OperationContext* opCtx) const override {
        MONGO_UNIMPLEMENTED_TASSERT(10445102);
    }

    void migrateRepresentativeQueriesFromDedicatedCollectionToQuerySettingsClusterParameter(
        OperationContext* opCtx) const override {
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
        const boost::optional<QuerySettings>& querySettingsFromOriginalCommand =
            boost::none) const override {
        // No query settings lookup for IDHACK queries.
        if (expCtx->isIdHackQuery()) {
            return QuerySettings();
        }

        auto* opCtx = expCtx->getOperationContext();
        if (requestComesFromRouterOrSentDirectlyToShard(opCtx->getClient()) ||
            querySettingsFromOriginalCommand.has_value()) {
            return querySettingsFromOriginalCommand.get_value_or(QuerySettings());
        }

        // The underlying shard does not belong to a sharded cluster, therefore proceed by
        // performing the lookup as a router.
        return QuerySettingsRouterService::lookupQuerySettingsWithRejectionCheck(
            expCtx, queryShapeHash, nss);
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

    void migrateRepresentativeQueriesFromQuerySettingsClusterParameterToDedicatedCollection(
        OperationContext* opCtx) const override {
        conflictingOperationInProgressRetry([&]() {
            std::vector<QueryShapeRepresentativeQuery> queryShapeRepresentativeQueries;
            auto config = getAllQueryShapeConfigurations(boost::none /* tenantId */);
            for (auto& queryShapeConfig : config.queryShapeConfigurations) {
                if (auto&& representativeQuery = queryShapeConfig.getRepresentativeQuery()) {
                    queryShapeRepresentativeQueries.emplace_back(
                        queryShapeConfig.getQueryShapeHash(),
                        *representativeQuery,
                        config.clusterParameterTime);

                    // Clear the representativeQuery information as it will be stored in a separate
                    // collection.
                    queryShapeConfig.setRepresentativeQuery(boost::none);
                }
            }

            // Early exit if there are no representative queries to migrate.
            if (queryShapeRepresentativeQueries.empty()) {
                return;
            }

            // Upsert all representative queries into the dedicated collection.
            upsertRepresentativeQueries(opCtx, queryShapeRepresentativeQueries);

            // Clear 'representativeQuery' info from QueryShapeConfiguration entries in
            // 'querySettings' cluster parameter.
            setQuerySettingsClusterParameter(opCtx, config);
        });
    }

    void migrateRepresentativeQueriesFromDedicatedCollectionToQuerySettingsClusterParameter(
        OperationContext* opCtx) const override {
        conflictingOperationInProgressRetry([&]() {
            // Read the QueryShapeConfigurations snapshot here as opposed to after opening the find
            // cursor to ensure that we detect the case of 'querySettings' cluster parameter
            // changes while having the cursor opened.
            auto config = getAllQueryShapeConfigurations(boost::none /* tenantId */);
            stdx::unordered_map<query_shape::QueryShapeHash, QueryInstance, QueryShapeHashHasher>
                representativeQueryMapping;

            int newQuerySettingsClusterParameterEstimatedBsonSize = [&]() {
                BSONObjBuilder bob;
                QuerySettingsClusterParameter p("querySettings"_sd,
                                                ServerParameterType::kClusterWide);
                p.append(opCtx, &bob, "", boost::none /* tenantId */);
                return bob.obj().objsize();
            }();

            // Issue a find request over queryShapeRepresentativeQueries collection, while sorting
            // over representativeQuery size in ascending order. We will try to migrate as many
            // representativeQueries back to the 'querySettings' cluster parameter as possible by
            // performing final 'querySettings' parameter size estimation.
            FindCommandRequest findRepresentativeQueries{
                NamespaceString::kQueryShapeRepresentativeQueriesNamespace};
            std::string dollarRepresentativeQuery = str::stream()
                << "$" << QueryShapeRepresentativeQuery::kRepresentativeQueryFieldName;
            findRepresentativeQueries.setProjection(
                BSON("_id" << 1 << QueryShapeRepresentativeQuery::kRepresentativeQueryFieldName << 1
                           << QueryShapeRepresentativeQuery::kLastModifiedTimeFieldName << 1
                           << "bsonSize" << BSON("$bsonSize" << dollarRepresentativeQuery)));
            findRepresentativeQueries.setSort(BSON("bsonSize" << -1));
            DBDirectClient client(opCtx);
            auto cursor = client.find(std::move(findRepresentativeQueries));
            while (cursor->more()) {
                BSONObj obj = cursor->next();
                auto representativeQueryBsonSize =
                    obj.getField(QueryShapeRepresentativeQuery::kRepresentativeQueryFieldName)
                        .objsize();

                // Do not read further from the cursor if total BSONObj size surpasses 16MB.
                if (newQuerySettingsClusterParameterEstimatedBsonSize +
                        representativeQueryBsonSize >
                    BSONObj::DefaultSizeTrait::MaxSize) {
                    cursor->kill();
                    break;
                }

                auto representativeQuery = QueryShapeRepresentativeQuery::parse(
                    obj, IDLParserContext{"QueryShapeRepresentativeQuery"});
                representativeQueryMapping.insert(
                    {representativeQuery.get_id(), representativeQuery.getRepresentativeQuery()});
                newQuerySettingsClusterParameterEstimatedBsonSize += representativeQueryBsonSize;
            }

            // Early exit if there are no representative queries to migrate.
            if (representativeQueryMapping.empty()) {
                return;
            }

            // Repopulate representative query information for the QueryShapeConfigurations.
            for (auto& queryShapeConfig : config.queryShapeConfigurations) {
                if (auto it = representativeQueryMapping.find(queryShapeConfig.getQueryShapeHash());
                    it != representativeQueryMapping.end()) {
                    queryShapeConfig.setRepresentativeQuery(std::move(it->second));
                }
            }

            // Try updating 'querySettings' cluster parameter. As we do accurate querySettings
            // cluster parameter size estimation, we do not expect BSONObjectTooLarge exception to
            // be thrown. But in case our estimation was incorrect, we do not retry and users will
            // lose representative queries, however, they will be backfilled, once users upgrade
            // their FCV.
            try {
                setQuerySettingsClusterParameter(opCtx, config);
            } catch (const ExceptionFor<ErrorCodes::BSONObjectTooLarge>&) {
                LOGV2_WARNING(10445109,
                              "Failed migrating representative queries to query settings storage");
            }
        });
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

const stdx::unordered_set<StringData, StringMapHasher>&
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
    // Query settings are allowed to be part of the request only in cases when request:
    // - comes from router (internal client), which has already performed the query settings lookup
    // or
    // - has been created interally and is executed via DBDirectClient.
    return requestComesFromRouterOrSentDirectlyToShard(client);
}

bool isDefault(const QuerySettings& settings) {
    // The 'serialization_context' and 'comment' fields are not significant.
    static_assert(QuerySettings::fieldNames.size() == 5,
                  "A new field has been added to the QuerySettings structure, isDefault should be "
                  "updated appropriately.");

    // For the 'reject' field of type OptionalBool, consider both 'false' and missing value as
    // default.
    return !(settings.getQueryFramework() || settings.getIndexHints() || settings.getReject());
}

void QuerySettingsService::validateQuerySettings(const QuerySettings& querySettings) const {
    // Validates that the settings field for query settings is not empty.
    uassert(7746604,
            "the resulting settings cannot be empty or contain only default values",
            !isDefault(querySettings));

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
