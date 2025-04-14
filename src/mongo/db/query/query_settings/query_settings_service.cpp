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

#include <boost/optional/optional.hpp>

#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/query/query_settings/query_settings_manager.h"
#include "mongo/db/query/query_shape/agg_cmd_shape.h"
#include "mongo/db/query/query_shape/distinct_cmd_shape.h"
#include "mongo/db/query/query_shape/find_cmd_shape.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/idl/cluster_server_parameter_refresher.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/serialization_context.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::query_settings {

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
    "$operationMetrics"_sd,
};

const auto getQuerySettingsService =
    ServiceContext::declareDecoration<std::unique_ptr<QuerySettingsService>>();

static constexpr auto kQuerySettingsClusterParameterName = "querySettings"_sd;

MONGO_FAIL_POINT_DEFINE(allowAllSetQuerySettings);

/**
 * If the pipeline starts with a "system"/administrative document source to which query settings
 * should not be applied, return the relevant stage name.
 */
boost::optional<std::string> getStageExemptedFromRejection(const std::vector<BSONObj> pipeline) {
    if (pipeline.empty()) {
        return boost::none;
    }

    if (canPipelineBeRejected(pipeline)) {
        // No pipeline stages are incompatible with rejection.
        return boost::none;
    }

    // Currently, all "system" queries are always the first stage in a pipeline.
    std::string firstStageName{pipeline.at(0).firstElementFieldName()};
    return {firstStageName};
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
                            "queryShapeHash"_attr = curOp->getQueryShapeHash()->toHexString(),
                            "ns"_attr = CurOp::get(opCtx)->getNS(),
                            "command"_attr = redact(cmdToLog.getObject()));
        uasserted(ErrorCodes::QueryRejectedBySettings, "Query rejected by admin query settings");
    }
}

/**
 * Sets query shape hash value 'hash' for the operation defined by 'opCtx' operation context.
 */
void setQueryShapeHash(OperationContext* opCtx, const QueryShapeHash& hash) {
    // Field 'queryShapeHash' is accessed by other threads therefore write the query shape hash
    // within a critical section.
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    CurOp::get(opCtx)->setQueryShapeHash(hash);
}

/*
 * Creates the corresponding RepresentativeQueryInfo for Find query representatives.
 */
RepresentativeQueryInfo createRepresentativeInfoFind(OperationContext* opCtx,
                                                     const QueryInstance& queryInstance,
                                                     const boost::optional<TenantId>& tenantId) {
    auto findCommandRequest = std::make_unique<FindCommandRequest>(
        FindCommandRequest::parse(IDLParserContext("findCommandRequest",
                                                   auth::ValidatedTenancyScope::get(opCtx),
                                                   tenantId,
                                                   kSerializationContext),
                                  queryInstance));

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

    auto expCtx = ExpressionContext::makeBlankExpressionContext(
        opCtx, nssOrUuid.nss(), findCommandRequest->getLet());
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
        DistinctCommandRequest::parse(IDLParserContext("distinctCommandRequest",
                                                       auth::ValidatedTenancyScope::get(opCtx),
                                                       tenantId,
                                                       kSerializationContext),
                                      queryInstance));
    // Extract namespace from distinct command.
    auto& nssOrUuid = distinctCommandRequest->getNamespaceOrUUID();
    uassert(7919501,
            "Collection namespace string must be provided for setQuerySettings command",
            nssOrUuid.isNamespaceString());

    auto expCtx = ExpressionContext::makeBlankExpressionContext(opCtx, nssOrUuid);
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
        AggregateCommandRequest::parse(IDLParserContext("aggregateCommandRequest",
                                                        auth::ValidatedTenancyScope::get(opCtx),
                                                        tenantId,
                                                        kSerializationContext),
                                       queryInstance);
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
    auto expCtx =
        ExpressionContext::makeBlankExpressionContext(opCtx, nss, aggregateCommandRequest.getLet());
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
}  // namespace

class QuerySettingsRouterService : public QuerySettingsService {
public:
    QuerySettingsRouterService() : QuerySettingsService() {}

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
        const query_shape::DeferredQueryShape& deferredShape,
        const NamespaceString& nss,
        const boost::optional<QuerySettings>& querySettingsFromOriginalCommand =
            boost::none) const override {
        QuerySettings settings = [&]() {
            try {
                // No query settings lookup for IDHACK queries.
                if (expCtx->isIdHackQuery()) {
                    return QuerySettings();
                }

                // No query settings for queries with encryption information.
                if (expCtx->isFleQuery()) {
                    return QuerySettings();
                }

                // No query settings lookup on internal dbs or system collections in user dbs.
                if (nss.isOnInternalDb() || nss.isSystem()) {
                    return QuerySettings();
                }

                // Force shape computation and early exit with empty settings if shape computation
                // has failed.
                const auto& shapePtr = deferredShape();
                if (!shapePtr.isOK()) {
                    return QuerySettings();
                }

                // Compute the QueryShapeHash and store it in CurOp.
                auto* opCtx = expCtx->getOperationContext();
                QueryShapeHash hash = shapePtr.getValue()->sha256Hash(opCtx, kSerializationContext);
                setQueryShapeHash(opCtx, hash);

                // Return the found query settings or an empty one.
                return _manager.getQuerySettingsForQueryShapeHash(hash, nss.tenantId())
                    .get_value_or({});
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

    void refreshQueryShapeConfigurations(OperationContext* opCtx) override {
        // QuerySettingsManager modifies a cluster-wide parameter and thus a refresh of
        // the parameter after that modification should observe results of preceeding
        // writes.
        const bool kEnsureReadYourWritesConsistency = true;
        auto refreshStatus = ClusterServerParameterRefresher::get(opCtx)->refreshParameters(
            opCtx, kEnsureReadYourWritesConsistency);
        if (!refreshStatus.isOK()) {
            LOGV2_WARNING(8472500,
                          "Error occurred when fetching the latest version of query settings",
                          "error_code"_attr = refreshStatus.code(),
                          "reason"_attr = refreshStatus.reason());
        }
    }

private:
    QuerySettingsManager _manager;
};

class QuerySettingsShardService : public QuerySettingsRouterService {
public:
    QuerySettingsShardService() : QuerySettingsRouterService() {}

    QuerySettings lookupQuerySettingsWithRejectionCheck(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const query_shape::DeferredQueryShape& queryShape,
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
            expCtx, queryShape, nss);
    }

    void refreshQueryShapeConfigurations(OperationContext* opCtx) override {
        /* no-op */
    }
};

QuerySettingsService& QuerySettingsService::get(ServiceContext* service) {
    return *getQuerySettingsService(service);
}

QuerySettingsService& QuerySettingsService::get(OperationContext* opCtx) {
    return *getQuerySettingsService(opCtx->getServiceContext());
}

std::string QuerySettingsService::getQuerySettingsClusterParameterName() {
    return kQuerySettingsClusterParameterName.toString();
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

void initializeForRouter(ServiceContext* serviceContext) {
    getQuerySettingsService(serviceContext) = std::make_unique<QuerySettingsRouterService>();
}

void initializeForShard(ServiceContext* serviceContext) {
    getQuerySettingsService(serviceContext) = std::make_unique<QuerySettingsShardService>();
}

void initializeForTest(ServiceContext* serviceContext) {
    initializeForShard(serviceContext);
}

QuerySettings lookupQuerySettingsWithRejectionCheckOnRouter(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const query_shape::DeferredQueryShape& deferredShape,
    const NamespaceString& nss) {
    auto* service =
        getQuerySettingsService(expCtx->getOperationContext()->getServiceContext()).get();
    dassert(dynamic_cast<QuerySettingsRouterService*>(service));
    return service->lookupQuerySettingsWithRejectionCheck(expCtx, deferredShape, nss);
}

QuerySettings lookupQuerySettingsWithRejectionCheckOnShard(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const query_shape::DeferredQueryShape& deferredShape,
    const NamespaceString& nss,
    const boost::optional<QuerySettings>& querySettingsFromOriginalCommand) {
    auto* service =
        getQuerySettingsService(expCtx->getOperationContext()->getServiceContext()).get();
    dassert(dynamic_cast<QuerySettingsShardService*>(service));
    return service->lookupQuerySettingsWithRejectionCheck(
        expCtx, deferredShape, nss, querySettingsFromOriginalCommand);
}

QueryShapeConfigurationsWithTimestamp getAllQueryShapeConfigurations(
    OperationContext* opCtx, const boost::optional<TenantId>& tenantId) {
    return QuerySettingsService::get(opCtx).getAllQueryShapeConfigurations(tenantId);
}

void refreshQueryShapeConfigurations(OperationContext* opCtx) {
    QuerySettingsService::get(opCtx).refreshQueryShapeConfigurations(opCtx);
}

std::string getQuerySettingsClusterParameterName() {
    return kQuerySettingsClusterParameterName.toString();
}

const stdx::unordered_set<StringData, StringMapHasher>& getRejectionIncompatibleStages() {
    return rejectionIncompatibleStages;
};

bool canPipelineBeRejected(const std::vector<BSONObj>& pipeline) {
    return pipeline.empty() ||
        !getRejectionIncompatibleStages().contains(pipeline.at(0).firstElementFieldName());
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

void validateQuerySettings(const QuerySettings& querySettings) {
    // Validates that the settings field for query settings is not empty.
    uassert(7746604,
            "the resulting settings cannot be empty or contain only default values",
            !isDefault(querySettings));

    validateQuerySettingsIndexHints(querySettings.getIndexHints());
}

void validateQueryCompatibleWithAnyQuerySettings(
    const RepresentativeQueryInfo& representativeQueryInfo) {
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

void validateQueryCompatibleWithQuerySettings(
    const RepresentativeQueryInfo& representativeQueryInfo, const QuerySettings& settings) {
    if (MONGO_unlikely(allowAllSetQuerySettings.shouldFail())) {
        return;
    }
    uassert(8705200,
            str::stream() << "Setting {reject:true} is forbidden for query containing stage: "
                          << *representativeQueryInfo.systemStage,
            !(settings.getReject() && representativeQueryInfo.systemStage.has_value()));
}

void simplifyQuerySettings(QuerySettings& settings) {
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
void sanitizeQuerySettingsHints(std::vector<QueryShapeConfiguration>& queryShapeConfigs) {
    std::erase_if(queryShapeConfigs, [](QueryShapeConfiguration& queryShapeItem) {
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
}  // namespace mongo::query_settings
