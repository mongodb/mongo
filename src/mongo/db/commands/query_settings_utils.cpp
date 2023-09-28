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

#include "mongo/db/commands/query_settings_utils.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/query/query_settings_manager.h"
#include "mongo/db/query/query_shape/agg_cmd_shape.h"
#include "mongo/db/query/query_shape/find_cmd_shape.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/util/serialization_context.h"

namespace mongo::query_settings {

namespace {
// Explicitly defines the `SerializationContext` to be used in `RepresentativeQueryInfo` factory
// methods. This was done as part of SERVER-79909 to ensure that inner query commands correctly
// infer the `tenantId`.
auto const kSerializationContext = SerializationContext{SerializationContext::Source::Command,
                                                        SerializationContext::CallerType::Request,
                                                        SerializationContext::Prefix::Default,
                                                        true /* nonPrefixedTenantId */};
}  // namespace

/*
 * Creates the corresponding RepresentativeQueryInfo for Find query representatives.
 */
RepresentativeQueryInfo createRepresentativeInfoFind(
    const QueryInstance& queryInstance,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::optional<TenantId>& tenantId) {
    auto findCommandRequest = std::make_unique<FindCommandRequest>(FindCommandRequest::parse(
        IDLParserContext(
            "findCommandRequest", false /* apiStrict */, tenantId, kSerializationContext),
        queryInstance));

    // Populate encryption information.
    auto& encryptionInformation = findCommandRequest->getEncryptionInformation();

    // Check if the find command is eligible for IDHACK.
    auto isIdHackEligibleQuery = isIdHackEligibleQueryWithoutCollator(*findCommandRequest);

    auto parsedFindCommand =
        uassertStatusOK(parsed_find_command::parse(expCtx, std::move(findCommandRequest)));

    // Extract namespace from find command.
    auto& nssOrUuid = parsedFindCommand->findCommandRequest->getNamespaceOrUUID();
    uassert(7746605,
            "Collection namespace string must be provided for setQuerySettings command",
            nssOrUuid.isNamespaceString());
    stdx::unordered_set<NamespaceString> involvedNamespaces{nssOrUuid.nss()};

    auto queryShapeHash =
        std::make_unique<query_shape::FindCmdShape>(*parsedFindCommand, expCtx)
            ->sha256Hash(expCtx->opCtx,
                         parsedFindCommand->findCommandRequest->getSerializationContext());

    return RepresentativeQueryInfo{std::move(queryShapeHash),
                                   std::move(involvedNamespaces),
                                   std::move(encryptionInformation),
                                   isIdHackEligibleQuery};
}

/*
 * Creates the corresponding RepresentativeQueryInfo for Aggregation query representatives.
 */
RepresentativeQueryInfo createRepresentativeInfoAgg(
    const QueryInstance& queryInstance,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::optional<TenantId>& tenantId) {
    auto aggregateCommandRequest = AggregateCommandRequest::parse(
        IDLParserContext(
            "aggregateCommandRequest", false /* apiStrict */, tenantId, kSerializationContext),
        queryInstance);
    // Add the aggregate command request to the expression context for the parsed pipeline
    // to be able to get the involved namespaces.
    expCtx->ns = aggregateCommandRequest.getNamespace();

    // Populate encryption information.
    auto& encryptionInformation = aggregateCommandRequest.getEncryptionInformation();

    // Populate foreign collection namespaces.
    auto parsedPipeline = LiteParsedPipeline(aggregateCommandRequest);
    auto involvedNamespaces = parsedPipeline.getInvolvedNamespaces();

    // When parsing the pipeline, we try to resolve the namespaces, which requires the resolved
    // namespaces to be present in the expression context.
    expCtx->addResolvedNamespaces(
        stdx::unordered_set<NamespaceString>{involvedNamespaces.begin(), involvedNamespaces.end()});

    // We also need to add the main namespace because 'addResolvedNamespaces' only
    // adds the foreign collections.
    auto resolvedNs = ExpressionContext::ResolvedNamespace{aggregateCommandRequest.getNamespace(),
                                                           aggregateCommandRequest.getPipeline()};
    involvedNamespaces.insert(resolvedNs.ns);

    auto pipeline = Pipeline::parse(aggregateCommandRequest.getPipeline(), expCtx);
    const auto& ns = aggregateCommandRequest.getNamespace();
    const auto sc = aggregateCommandRequest.getSerializationContext();
    auto queryShapeHash =
        std::make_unique<query_shape::AggCmdShape>(
            std::move(aggregateCommandRequest), ns, involvedNamespaces, *pipeline, expCtx)
            ->sha256Hash(expCtx->opCtx, sc);

    // For aggregate queries, the check for IDHACK should not be taken into account due to the
    // complexity of determining if a pipeline is eligible or not for IDHACK.
    return RepresentativeQueryInfo{std::move(queryShapeHash),
                                   std::move(involvedNamespaces),
                                   std::move(encryptionInformation),
                                   false};
}

// TODO: SERVER-79105 Ensure setQuerySettings and removeQuerySettings commands can take distinct
// commands as arguments - a similar function can be added for `distinct` command.

RepresentativeQueryInfo createRepresentativeInfo(
    const BSONObj& cmd,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::optional<TenantId>& tenantId) {
    if (cmd.firstElementFieldName() == FindCommandRequest::kCommandName) {
        return createRepresentativeInfoFind(cmd, expCtx, tenantId);
    }
    if (cmd.firstElementFieldName() == AggregateCommandRequest::kCommandName) {
        return createRepresentativeInfoAgg(cmd, expCtx, tenantId);
    }
    uasserted(7746402, str::stream() << "QueryShape can not be computed for command: " << cmd);
}

namespace utils {

/**
 * Returns the namespace field of the hint, in case it is present. In case it is not present, it
 * returns the infered namespace or throws an error if multiple collections are involved.
 */
NamespaceString getHintNamespace(const mongo::query_settings::IndexHintSpec& hint,
                                 const stdx::unordered_set<NamespaceString>& namespacesSet,
                                 const boost::optional<TenantId>& tenantId) {
    tassert(7746607, "involved namespaces cannot be empty!", !namespacesSet.empty());
    const auto& ns = hint.getNs();
    if (ns) {
        return NamespaceStringUtil::deserialize(
            tenantId, ns->getDb(), ns->getColl(), SerializationContext::stateDefault());
    }
    uassert(7746602,
            str::stream() << "Hint: '" << hint.toBSON().toString()
                          << "' does not contain a namespace field and more than one "
                             "collection is involved the query",
            namespacesSet.size() == 1);
    // In case the namespace is not defined but there is only one collection involved,
    // we can infere the namespace.
    return *namespacesSet.begin();
}

/**
 * Validates that `setQuerySettings` command namespace can naïvely be deduced
 * from the query shape if only one collection is involved. In case multiple collections are
 * involved, the method ensures that each index hint is used at most once.
 *
 * This method also checks that every index hint namespace specified refers to an “involved”
 * collection and that two index hints cannot refer to the same collection.
 */
void validateQuerySettingsNamespacesNotAmbiguous(
    const QueryShapeConfiguration& config,
    const RepresentativeQueryInfo& representativeQueryInfo,
    const boost::optional<TenantId>& tenantId) {
    // If there are no index hints involved, no validation is required.
    if (!config.getSettings().getIndexHints()) {
        return;
    }

    auto hints = stdx::visit(
        OverloadedVisitor{
            [](const std::vector<mongo::query_settings::IndexHintSpec>& hints) { return hints; },
            [](const mongo::query_settings::IndexHintSpec& hints) { return std::vector{hints}; },
        },
        (*config.getSettings().getIndexHints()));

    auto& namespacesSet = representativeQueryInfo.involvedNamespaces;
    stdx::unordered_map<NamespaceString, mongo::query_settings::IndexHintSpec>
        collectionsWithAppliedIndexHints;
    for (const auto& hint : hints) {
        NamespaceString nss = getHintNamespace(hint, namespacesSet, tenantId);

        uassert(7746603,
                str::stream() << "Namespace: '" << nss.toStringForErrorMsg()
                              << "' does not refer to any involved collection",
                namespacesSet.contains(nss));

        auto [it, emplaced] = collectionsWithAppliedIndexHints.emplace(nss, hint);
        uassert(7746608,
                str::stream() << "Collections can be applied at most one index hint, but indices '"
                              << collectionsWithAppliedIndexHints[nss].toBSON().toString()
                              << "' and '" << hint.toBSON().toString()
                              << "' refer to the same collection",
                emplaced);
    }
}

/**
 * Validates that QueryShapeConfiguration is not specified for queries with queryable encryption.
 */
void validateQuerySettingsEncryptionInformation(
    const QueryShapeConfiguration& config, const RepresentativeQueryInfo& representativeQueryInfo) {
    uassert(7746600,
            "Queries with encryption information are not allowed on setQuerySettings commands",
            !representativeQueryInfo.encryptionInformation);

    bool containsFLE2StateCollection =
        std::any_of(representativeQueryInfo.involvedNamespaces.begin(),
                    representativeQueryInfo.involvedNamespaces.end(),
                    [](const NamespaceString& ns) { return ns.isFLE2StateCollection(); });

    uassert(7746601,
            "setQuerrySettings command is not allowed on queryable encryption state collections",
            !containsFLE2StateCollection);
}

void validateQuerySettings(const QueryShapeConfiguration& config,
                           const RepresentativeQueryInfo& representativeQueryInfo,
                           const boost::optional<TenantId>& tenantId) {
    // Validates that the settings field for query settings is not empty.
    uassert(7746604,
            "settings field in setQuerrySettings command cannot be empty",
            !config.getSettings().toBSON().isEmpty());

    validateQuerySettingsEncryptionInformation(config, representativeQueryInfo);

    // Validates that the query settings' representative is not eligible for IDHACK.
    uassert(7746606,
            "setQuerrySettings command cannot be used on find queries eligible for IDHACK",
            !representativeQueryInfo.isIdHackQuery);

    validateQuerySettingsNamespacesNotAmbiguous(config, representativeQueryInfo, tenantId);
}

}  // namespace utils
}  // namespace mongo::query_settings
