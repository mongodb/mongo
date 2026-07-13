// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {
class AggregateCommandRequest;
class DistinctCommandRequest;
class FindCommandRequest;

class ExpressionContextBuilder {
public:
    // When building an ExpressionContext, call exactly one of these 'opCtx()' overloads.
    //
    // Set the OperationContext and then initialize a VersionContext based on the OperationContext's
    // VersionContext when it has an operation FCV and the current global
    // FeatureCompatibilityVersion otherwise.
    ExpressionContextBuilder& opCtx(OperationContext*);

    // Set the OperationContext and initialize the VersionContext without considering the
    // OperationContext's VersionContext or the global FeatureCompatibilityVersion. Intended for
    // ExpressionContext cloning.
    ExpressionContextBuilder& opCtx(OperationContext*, VersionContext vCtx);

    ExpressionContextBuilder& ifrContext(
        std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext);
    ExpressionContextBuilder& collator(std::unique_ptr<CollatorInterface>&&);
    ExpressionContextBuilder& mongoProcessInterface(std::shared_ptr<MongoProcessInterface>);
    ExpressionContextBuilder& ns(NamespaceString);
    ExpressionContextBuilder& resolvedNamespace(ResolvedNamespaceMap);
    ExpressionContextBuilder& serializationContext(SerializationContext);
    ExpressionContextBuilder& collUUID(boost::optional<UUID>);
    ExpressionContextBuilder& explain(boost::optional<ExplainOptions::Verbosity>);
    ExpressionContextBuilder& runtimeConstants(boost::optional<LegacyRuntimeConstants>);
    ExpressionContextBuilder& letParameters(boost::optional<BSONObj>);
    ExpressionContextBuilder& tmpDir(boost::filesystem::path);
    ExpressionContextBuilder& mayDbProfile(bool);
    ExpressionContextBuilder& fromRouter(bool);
    ExpressionContextBuilder& mergeType(MergeType);
    ExpressionContextBuilder& inRouter(bool);
    ExpressionContextBuilder& forPerShardCursor(bool);
    ExpressionContextBuilder& excludeOperationMemoryTracking(bool);
    ExpressionContextBuilder& allowDiskUse(bool);
    ExpressionContextBuilder& allowPartialResults(bool);
    ExpressionContextBuilder& bypassDocumentValidation(bool);
    ExpressionContextBuilder& isMapReduceCommand(bool);
    ExpressionContextBuilder& hasWhereClause(bool);
    ExpressionContextBuilder& isUpsert(bool);
    ExpressionContextBuilder& blankExpressionContext(bool);
    ExpressionContextBuilder& collationMatchesDefault(ExpressionContextCollationMatchesDefault);
    ExpressionContextBuilder& inLookup(bool);
    ExpressionContextBuilder& inUnionWith(bool);
    ExpressionContextBuilder& isParsingViewDefinition(bool);
    ExpressionContextBuilder& isParsingCollectionValidator(bool);
    ExpressionContextBuilder& isIdHackQuery(bool);
    ExpressionContextBuilder& isFleQuery(bool);
    ExpressionContextBuilder& canBeRejected(bool);
    ExpressionContextBuilder& exprUnstableForApiV1(bool);
    ExpressionContextBuilder& exprDeprecatedForApiV1(bool);
    ExpressionContextBuilder& enabledCounters(bool);
    ExpressionContextBuilder& planCache(ExpressionContext::PlanCacheOptions);
    ExpressionContextBuilder& allowGenericForeignDbLookup(bool);
    ExpressionContextBuilder& requiresTimeseriesExtendedRangeSupport(bool);
    ExpressionContextBuilder& jsHeapLimitMB(boost::optional<int>);
    ExpressionContextBuilder& timeZoneDatabase(const TimeZoneDatabase*);
    ExpressionContextBuilder& changeStreamTokenVersion(int);
    ExpressionContextBuilder& changeStreamSpec(boost::optional<DocumentSourceChangeStreamSpec>);
    ExpressionContextBuilder& originalAggregateCommand(BSONObj);
    ExpressionContextBuilder& sbeCompatibility(SbeCompatibility);
    ExpressionContextBuilder& sbeGroupCompatibility(SbeCompatibility);
    ExpressionContextBuilder& sbeWindowCompatibility(SbeCompatibility);
    ExpressionContextBuilder& sbePipelineCompatibility(SbeCompatibility);
    ExpressionContextBuilder& serverSideJsConfig(const ExpressionContext::ServerSideJsConfig&);
    ExpressionContextBuilder& subPipelineDepth(long long);
    ExpressionContextBuilder& initialPostBatchResumeToken(BSONObj);
    ExpressionContextBuilder& tailableMode(TailableModeEnum);
    ExpressionContextBuilder& view(boost::optional<ResolvedNamespace>);
    ExpressionContextBuilder& originalNs(NamespaceString);
    ExpressionContextBuilder& isHybridSearch(bool);
    ExpressionContextBuilder& pathArraynessForNss(
        stdx::unordered_map<NamespaceString, std::shared_ptr<const PathArrayness>>
            pathArraynessForNss);
    ExpressionContextBuilder& pathArraynessFrom(const ExpressionContext& other);
    ExpressionContextBuilder& nonArrayPathsForNssFrom(const ExpressionContext& other);

    /**
     * Add kSessionTransactionsTableNamespace, and kRsOplogNamespace
     * to resolvedNamespaces since they are all used during different pipeline stages
     */
    ExpressionContextBuilder& withReplicationResolvedNamespaces();

    ExpressionContextBuilder& fromRequest(OperationContext*,
                                          const FindCommandRequest&,
                                          const CollatorInterface*,
                                          bool useDisk = false);
    ExpressionContextBuilder& fromRequest(OperationContext*,
                                          const FindCommandRequest&,
                                          bool dbProfile = true);
    ExpressionContextBuilder& fromRequest(OperationContext*,
                                          const DistinctCommandRequest&,
                                          const CollatorInterface* = nullptr);
    ExpressionContextBuilder& fromRequest(OperationContext*,
                                          const AggregateCommandRequest&,
                                          bool useDisk = false);
    ExpressionContextBuilder& fromRequest(OperationContext*,
                                          const UpdateRequest& request,
                                          bool forgoOpCounterIncrements = false);
    ExpressionContextBuilder& fromRequest(OperationContext*, const DeleteRequest& request);

    boost::intrusive_ptr<ExpressionContext> build();

private:
    ExpressionContext::ExpressionContextParams params;
};

/**
 * Constructs a blank ExpressionContext suitable for creating Query Shapes, but it could be
 * applied to other use cases as well.
 *
 * The process for creating a Query Shape sometimes requires re-parsing the BSON into a proper
 * AST, and for that you need an ExpressionContext.
 *
 * Note: this is meant for introspection and is not suitable for using to execute queries -
 * since it does not contain for example a collation argument or a real MongoProcessInterface
 * for execution.
 */
boost::intrusive_ptr<ExpressionContext> makeBlankExpressionContext(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nssOrUUID,
    boost::optional<BSONObj> shapifiedLet = boost::none);

/**
 * Returns an ExpressionContext that is identical to 'expCtx' that can be used to execute a
 * separate aggregation pipeline on 'ns' with the optional 'uuid' and an updated collator.
 * 'userNs' indicates the namespace the user typed in the 'from/coll' field in a
 * $lookup/$unionWith respectively.
 */
boost::intrusive_ptr<ExpressionContext> makeCopyFromExpressionContext(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    NamespaceString ns,
    boost::optional<UUID> uuid = boost::none,
    boost::optional<std::unique_ptr<CollatorInterface>> updatedCollator = boost::none,
    const boost::optional<ResolvedNamespace>& view = boost::none,
    boost::optional<NamespaceString> userNs = boost::none);

/**
 * Returns an ExpressionContext that is identical to 'expCtx' except for the 'subPipelineDepth'
 * and 'needsMerge' fields, as well as changing the 'userNs' to whatever was passed in as
 * 'from' in $lookup or 'coll' in $unionWith.
 */
boost::intrusive_ptr<ExpressionContext> makeCopyForSubPipelineFromExpressionContext(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    NamespaceString nss,
    boost::optional<UUID> uuid = boost::none,
    boost::optional<NamespaceString> userNs = boost::none);
}  // namespace mongo
