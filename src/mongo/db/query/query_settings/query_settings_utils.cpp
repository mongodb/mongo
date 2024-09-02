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

#include "mongo/db/query/query_settings/query_settings_utils.h"

#include <boost/optional/optional.hpp>
#include <string_view>

#include "mongo/db/curop.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_settings/query_settings_manager.h"
#include "mongo/db/query/query_shape/agg_cmd_shape.h"
#include "mongo/db/query/query_shape/distinct_cmd_shape.h"
#include "mongo/db/query/query_shape/find_cmd_shape.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/serialization_context.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::query_settings {

using namespace query_shape;

namespace {
// Explicitly defines the `SerializationContext` to be used in `RepresentativeQueryInfo` factory
// methods. This was done as part of SERVER-79909 to ensure that inner query commands correctly
// infer the `tenantId`.
auto const kSerializationContext =
    SerializationContext{SerializationContext::Source::Command,
                         SerializationContext::CallerType::Request,
                         SerializationContext::Prefix::ExcludePrefix};

MONGO_FAIL_POINT_DEFINE(allowAllSetQuerySettings);

void failIfRejectedBySettings(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              const QuerySettings& settings) {
    if (expCtx->explain) {
        // Explaining queries which _would_ be rejected if executed is still useful;
        // do not fail here.
        return;
    }
    if (settings.getReject()) {
        auto* opCtx = expCtx->opCtx;
        const Command* curCommand = CommandInvocation::get(opCtx)->definition();
        auto* curOp = CurOp::get(opCtx);
        auto query = curOp->opDescription();

        mutablebson::Document cmdToLog(query, mutablebson::Document::kInPlaceDisabled);
        curCommand->snipForLogging(&cmdToLog);
        LOGV2_DEBUG_OPTIONS(8687100,
                            2,
                            {logv2::LogComponent::kQueryRejected},
                            "Query rejected by QuerySettings",
                            "queryShapeHash"_attr = curOp->getQueryShapeHash()->toHexString(),
                            "ns"_attr = CurOp::get(expCtx->opCtx)->getNS(),
                            "command"_attr = redact(cmdToLog.getObject()));
        uasserted(ErrorCodes::QueryRejectedBySettings, "Query rejected by admin query settings");
    }
}

/**
 * System or administrative queries should not be rejected, even if a user
 * chooses to set reject=true.
 */
bool pipelineCanBeRejected(const Pipeline& pipeline) {
    using namespace std::string_view_literals;
    static const stdx::unordered_set<std::string_view> exemptedStages = {
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
        "$operationMetrics"sv,
    };
    return !pipeline.peekFront() || !exemptedStages.contains(pipeline.peekFront()->getSourceName());
}

/**
 * Aggregate commands require additional introspection to decide if the pipeline is suitable for
 * rejection to apply.
 *
 * "System" requests (used internally or for administration) are permitted to ignore reject, to
 * avoid accidentally reaching a hard to resolve state, or breaking internal mechanisms.
 */
void failIfRejectedBySettings(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              const Pipeline& pipeline,
                              const QuerySettings& settings) {
    // Agg requests with "system" stages like $querySettings should not be failed,
    // even if reject has been set by query hash.
    if (!pipelineCanBeRejected(pipeline)) {
        return;
    }
    // Continue on to the common checks, and maybe fail the request.
    failIfRejectedBySettings(expCtx, settings);
}

/**
 * If the pipeline starts with a "system"/administrative document source to which query settings
 * should not be applied, return the relevant stage name.
 */
boost::optional<std::string> getStageExemptedFromRejection(const Pipeline& pipeline) {
    if (pipelineCanBeRejected(pipeline)) {
        // No pipeline stages are incompatible with rejection.
        return boost::none;
    }

    const auto* firstStage = pipeline.peekFront();
    // An empty pipeline should have lead to the above early exit being taken.
    tassert(8705201, "Empty pipeline should be eligible for rejection, but was not", firstStage);

    // Currently, all "system" queries are always the first stage in a pipeline.
    return {std::string(firstStage->getSourceName())};
}

/**
 * Sets query shape hash value 'hash' for the operation defined by 'opCtx' operation context.
 */
void setQueryShapeHash(OperationContext* opCtx, const boost::optional<QueryShapeHash>& hash) {
    // Field 'queryShapeHash' is accessed by other threads therefore write the query shape hash
    // within a critical section.
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    CurOp::get(opCtx)->setQueryShapeHash(hash);
}
}  // namespace

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
                                       queryInstance,
                                       std::move(distinctCommandRequest),
                                       ExtensionsCallbackNoop(),
                                       MatchExpressionParser::kAllowAllSpecialFeatures);


    DistinctCmdShape distinctCmdShape{*parsedDistinctCommand, expCtx};
    const auto serializationContext =
        parsedDistinctCommand->distinctCommandRequest->getSerializationContext();
    auto serializedQueryShape =
        distinctCmdShape.toBson(expCtx->opCtx,
                                SerializationOptions::kDebugQueryShapeSerializeOptions,
                                serializationContext);

    return RepresentativeQueryInfo{
        .serializedQueryShape = serializedQueryShape,
        .queryShapeHash = distinctCmdShape.sha256Hash(expCtx->opCtx, serializationContext),
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
    auto resolvedNs = ExpressionContext::ResolvedNamespace{aggregateCommandRequest.getNamespace(),
                                                           aggregateCommandRequest.getPipeline()};
    involvedNamespaces.insert(resolvedNs.ns);

    // When parsing the pipeline, we try to resolve the namespaces, which requires the resolved
    // namespaces to be present in the expression context.
    auto nss = aggregateCommandRequest.getNamespace();
    auto expCtx =
        ExpressionContext::makeBlankExpressionContext(opCtx, nss, aggregateCommandRequest.getLet());
    expCtx->addResolvedNamespaces(
        stdx::unordered_set<NamespaceString>{involvedNamespaces.begin(), involvedNamespaces.end()});
    auto pipeline = Pipeline::parse(aggregateCommandRequest.getPipeline(), expCtx);

    const auto serializationContext = aggregateCommandRequest.getSerializationContext();
    AggCmdShape aggCmdShape{aggregateCommandRequest, nss, involvedNamespaces, *pipeline, expCtx};
    auto serializedQueryShape =
        aggCmdShape.toBson(expCtx->opCtx,
                           SerializationOptions::kDebugQueryShapeSerializeOptions,
                           serializationContext);

    // For aggregate queries, the check for IDHACK should not be taken into account due to the
    // complexity of determining if a pipeline is eligible or not for IDHACK.
    return RepresentativeQueryInfo{
        .serializedQueryShape = serializedQueryShape,
        .queryShapeHash = aggCmdShape.sha256Hash(expCtx->opCtx, serializationContext),
        .namespaceString = nss,
        .involvedNamespaces = std::move(involvedNamespaces),
        .encryptionInformation = aggregateCommandRequest.getEncryptionInformation(),
        .isIdHackQuery = false,
        .systemStage = getStageExemptedFromRejection(*pipeline),
    };
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

QuerySettings lookupQuerySettingsForFind(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         const ParsedFindCommand& parsedFind,
                                         const NamespaceString& nss) {
    // No query settings lookup for IDHACK queries.
    if (isIdHackEligibleQueryWithoutCollator(*parsedFind.findCommandRequest)) {
        return query_settings::QuerySettings();
    }

    // No query settings lookup on internal dbs or system collections in user dbs.
    if (nss.isOnInternalDb() || nss.isSystem()) {
        return query_settings::QuerySettings();
    }

    // No query settings for queries with encryption information.
    if (parsedFind.findCommandRequest->getEncryptionInformation()) {
        return query_settings::QuerySettings();
    }

    // If query settings are present as part of the request, use them as opposed to performing the
    // query settings lookup. In this case, no check for 'reject' setting will be made.
    if (auto querySettings = parsedFind.findCommandRequest->getQuerySettings()) {
        return *querySettings;
    }

    // We need to use isEnabledUseLatestFCVWhenUninitialized instead of isEnabled because
    // this could run during startup while the FCV is still uninitialized.
    if (!feature_flags::gFeatureFlagQuerySettings.isEnabledUseLatestFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return query_settings::QuerySettings();
    }

    auto* opCtx = expCtx->opCtx;
    const auto& serializationContext = parsedFind.findCommandRequest->getSerializationContext();
    auto curOp = CurOp::get(opCtx);
    auto& opDebug = curOp->debug();
    auto hash = [&]() -> boost::optional<QueryShapeHash> {
        if (opDebug.queryStatsInfo.key) {
            return opDebug.queryStatsInfo.key->getQueryShapeHash(opCtx, serializationContext);
        }
        boost::optional<query_shape::FindCmdShape> shape;
        try {
            shape.emplace(parsedFind, expCtx);
        } catch (ExceptionFor<ErrorCodes::BSONObjectTooLarge>&) {
            return boost::none;
        }
        return shape->sha256Hash(expCtx->opCtx, serializationContext);
    }();
    if (!hash) {
        return query_settings::QuerySettings();
    }
    setQueryShapeHash(opCtx, hash);

    // Return the found query settings or an empty one.
    auto& manager = QuerySettingsManager::get(opCtx);
    auto settings = manager.getQuerySettingsForQueryShapeHash(opCtx, *hash, nss.dbName().tenantId())
                        .get_value_or({});

    // Fail the current command, if 'reject: true' flag is present.
    failIfRejectedBySettings(expCtx, settings);

    return settings;
}

QuerySettings lookupQuerySettingsForAgg(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const AggregateCommandRequest& aggregateCommandRequest,
    const Pipeline& pipeline,
    const stdx::unordered_set<NamespaceString>& involvedNamespaces,
    const NamespaceString& nss) {
    // No query settings lookup on internal dbs or system collections in user dbs.
    if (nss.isOnInternalDb() || nss.isSystem()) {
        return query_settings::QuerySettings();
    }

    // No query settings for queries with encryption information.
    if (aggregateCommandRequest.getEncryptionInformation()) {
        return query_settings::QuerySettings();
    }

    // If query settings are present as part of the request, use them as opposed to performing the
    // query settings lookup. In this case, no check for 'reject' setting will be made.
    if (auto querySettings = aggregateCommandRequest.getQuerySettings()) {
        return *querySettings;
    }

    // We need to use isEnabledUseLatestFCVWhenUninitialized instead of isEnabled because
    // this could run during startup while the FCV is still uninitialized.
    if (!feature_flags::gFeatureFlagQuerySettings.isEnabledUseLatestFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return query_settings::QuerySettings();
    }

    auto* opCtx = expCtx->opCtx;
    const auto& serializationContext = aggregateCommandRequest.getSerializationContext();
    auto curOp = CurOp::get(opCtx);
    auto& opDebug = curOp->debug();
    auto hash = [&]() -> boost::optional<QueryShapeHash> {
        if (opDebug.queryStatsInfo.key) {
            return opDebug.queryStatsInfo.key->getQueryShapeHash(opCtx, serializationContext);
        }
        boost::optional<query_shape::AggCmdShape> shape;
        try {
            shape.emplace(aggregateCommandRequest, nss, involvedNamespaces, pipeline, expCtx);
        } catch (ExceptionFor<ErrorCodes::BSONObjectTooLarge>&) {
            return boost::none;
        }
        return shape->sha256Hash(opCtx, serializationContext);
    }();
    if (!hash) {
        return query_settings::QuerySettings();
    }
    setQueryShapeHash(opCtx, hash);

    // Return the found query settings or an empty one.
    auto& manager = QuerySettingsManager::get(opCtx);
    auto settings = manager.getQuerySettingsForQueryShapeHash(opCtx, *hash, nss.dbName().tenantId())
                        .get_value_or({});

    // Fail the current command, if 'reject: true' flag is present.
    failIfRejectedBySettings(expCtx, pipeline, settings);

    return settings;
}

QuerySettings lookupQuerySettingsForDistinct(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                             const ParsedDistinctCommand& parsedDistinct,
                                             const NamespaceString& nss) {
    // No query settings lookup on internal dbs or system collections in user dbs.
    if (nss.isOnInternalDb() || nss.isSystem()) {
        return query_settings::QuerySettings();
    }

    // If query settings are present as part of the request, use them as opposed to performing the
    // query settings lookup. In this case, no check for 'reject' setting will be made.
    if (auto querySettings = parsedDistinct.distinctCommandRequest->getQuerySettings()) {
        return *querySettings;
    }

    // We need to use isEnabledUseLatestFCVWhenUninitialized instead of isEnabled because
    // this could run during startup while the FCV is still uninitialized.
    if (!feature_flags::gFeatureFlagQuerySettings.isEnabledUseLatestFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return query_settings::QuerySettings();
    }

    auto* opCtx = expCtx->opCtx;
    const auto& serializationContext =
        parsedDistinct.distinctCommandRequest->getSerializationContext();
    auto curOp = CurOp::get(opCtx);
    auto& opDebug = curOp->debug();
    auto hash = [&]() -> boost::optional<QueryShapeHash> {
        if (opDebug.queryStatsInfo.key) {
            return opDebug.queryStatsInfo.key->getQueryShapeHash(opCtx, serializationContext);
        }
        boost::optional<query_shape::DistinctCmdShape> shape;
        try {
            shape.emplace(parsedDistinct, expCtx);
        } catch (ExceptionFor<ErrorCodes::BSONObjectTooLarge>&) {
            return boost::none;
        }
        return shape->sha256Hash(expCtx->opCtx, serializationContext);
    }();

    if (!hash) {
        return query_settings::QuerySettings();
    }
    setQueryShapeHash(opCtx, hash);

    // Return the found query settings or an empty one.
    auto& manager = QuerySettingsManager::get(opCtx);
    auto settings = manager.getQuerySettingsForQueryShapeHash(opCtx, *hash, nss.dbName().tenantId())
                        .get_value_or({});

    // Fail the current command, if 'reject: true' flag is present.
    failIfRejectedBySettings(expCtx, settings);

    return settings;
}

namespace utils {

bool allowQuerySettingsFromClient(Client* client) {
    // Query settings are allowed to be part of the request only in cases when request:
    // - comes from mongos (internal client), which has already performed the query settings lookup
    // or
    // - has been created interally and is executed via DBDirectClient.
    return client->isInternalClient() || client->isInDirectClient();
}

bool isDefault(const QuerySettings& settings) {
    // The 'serialization_context' field is not significant.
    static_assert(QuerySettings::fieldNames.size() == 5,
                  "A new field has been added to the QuerySettings structure, isDefault should be "
                  "updated appropriately.");

    // For the 'reject' field of type OptionalBool, consider both 'false' and missing value as
    // default.
    return !(settings.getQueryFramework() || settings.getIndexHints() || settings.getReject());
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
        auto nss = NamespaceStringUtil::deserialize(*hint.getNs().getDb(), *hint.getNs().getColl());
        auto [it, emplaced] = collectionsWithAppliedIndexHints.emplace(nss, hint);
        uassert(7746608,
                str::stream() << "Collection '"
                              << collectionsWithAppliedIndexHints[nss].toBSON().toString()
                              << "' has already index hints specified",
                emplaced);
    }
}

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

void validateRepresentativeQuery(const RepresentativeQueryInfo& representativeQueryInfo) {
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

void validateQuerySettings(const QuerySettings& querySettings) {
    // Validates that the settings field for query settings is not empty.
    uassert(7746604,
            "the resulting settings cannot be empty or contain only default values",
            !isDefault(querySettings));

    validateQuerySettingsIndexHints(querySettings.getIndexHints());
}

void verifyQueryCompatibleWithSettings(const RepresentativeQueryInfo& representativeQueryInfo,
                                       const QuerySettings& settings) {
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

}  // namespace utils
}  // namespace mongo::query_settings
