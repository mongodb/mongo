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
#include "mongo/db/pipeline/expression_context_builder.h"

#include "mongo/db/curop.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/query_utils.h"

namespace mongo {

ExpressionContextBuilder& ExpressionContextBuilder::opCtx(OperationContext* opCtx) {
    params.opCtx = opCtx;

    VersionContext vCtx = VersionContext::getDecoration(opCtx);
    if (vCtx.isInitialized()) {
        params.vCtx = vCtx;
    } else if (auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
               fcvSnapshot.isVersionInitialized()) {
        params.vCtx.setOperationFCV(fcvSnapshot);
    } else {
        // Leave 'vCtx' in its uninitialized state.
    }

    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::opCtx(OperationContext* opCtx,
                                                          VersionContext vCtx) {
    params.opCtx = opCtx;
    params.vCtx = vCtx;

    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::ifrContext(
    const IncrementalFeatureRolloutContext& ifrContext) {
    params.ifrContext = ifrContext;

    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::collator(
    std::unique_ptr<CollatorInterface>&& collator) {
    params.collator = std::move(collator);
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::mongoProcessInterface(
    std::shared_ptr<MongoProcessInterface> processInterface) {
    params.mongoProcessInterface = std::move(processInterface);
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::ns(NamespaceString ns) {
    params.ns = std::move(ns);
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::resolvedNamespace(
    ResolvedNamespaceMap resolvedNamespaces) {
    params.resolvedNamespaces = std::move(resolvedNamespaces);
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::serializationContext(
    SerializationContext serializationContext) {
    params.serializationContext = std::move(serializationContext);
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::tmpDir(boost::filesystem::path tmpDir) {
    params.tmpDir = std::move(tmpDir);
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::mayDbProfile(bool mayDBProfile) {
    params.mayDbProfile = mayDBProfile;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::fromRouter(bool fromRouter) {
    params.fromRouter = fromRouter;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::mergeType(MergeType mergeType) {
    params.mergeType = mergeType;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::inRouter(bool inRouter) {
    params.inRouter = inRouter;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::forPerShardCursor(bool forPerShardCursor) {
    params.forPerShardCursor = forPerShardCursor;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::allowDiskUse(bool allowDiskUse) {
    params.allowDiskUse = allowDiskUse;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::bypassDocumentValidation(
    bool bypassDocumentValidation) {
    params.bypassDocumentValidation = bypassDocumentValidation;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::isMapReduceCommand(bool isMapReduce) {
    params.isMapReduceCommand = isMapReduce;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::hasWhereClause(bool hasWhereClause) {
    params.hasWhereClause = hasWhereClause;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::isUpsert(bool upsert) {
    params.isUpsert = upsert;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::inLookup(bool inLookup) {
    params.inLookup = inLookup;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::inUnionWith(bool inUnionWith) {
    params.inUnionWith = inUnionWith;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::isParsingViewDefinition(
    bool isParsingViewDefinition) {
    params.isParsingViewDefinition = isParsingViewDefinition;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::isParsingPipelineUpdate(
    bool isParsingPipelineUpdate) {
    params.isParsingPipelineUpdate = isParsingPipelineUpdate;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::isParsingCollectionValidator(
    bool isParsingCollectionValidator) {
    params.isParsingCollectionValidator = isParsingCollectionValidator;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::isIdHackQuery(bool isIdHackQuery) {
    params.isIdHackQuery = isIdHackQuery;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::isFleQuery(bool isFleQuery) {
    params.isFleQuery = isFleQuery;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::canBeRejected(bool canBeRejected) {
    params.canBeRejected = canBeRejected;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::blankExpressionContext(
    bool blankExpressionContext) {
    params.blankExpressionContext = blankExpressionContext;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::exprUnstableForApiV1(
    bool exprUnstableForApiV1) {
    params.exprUnstableForApiV1 = exprUnstableForApiV1;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::exprDeprecatedForApiV1(
    bool exprDeprecatedForApiV1) {
    params.exprDeprecatedForApiV1 = exprDeprecatedForApiV1;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::enabledCounters(bool enabledCounters) {
    params.enabledCounters = enabledCounters;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::forcePlanCache(bool forcePlanCache) {
    params.forcePlanCache = forcePlanCache;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::allowGenericForeignDbLookup(
    bool allowGenericForeignDbLookup) {
    params.allowGenericForeignDbLookup = allowGenericForeignDbLookup;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::requiresTimeseriesExtendedRangeSupport(
    bool requiresTimeseriesExtendedRangeSupport) {
    params.requiresTimeseriesExtendedRangeSupport = requiresTimeseriesExtendedRangeSupport;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::collUUID(boost::optional<UUID> collUUID) {
    params.collUUID = std::move(collUUID);
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::explain(
    boost::optional<ExplainOptions::Verbosity> explain) {
    params.explain = std::move(explain);
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::runtimeConstants(
    boost::optional<LegacyRuntimeConstants> runtimeConstants) {
    params.runtimeConstants = std::move(runtimeConstants);
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::letParameters(
    boost::optional<BSONObj> letParameters) {
    params.letParameters = std::move(letParameters);
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::jsHeapLimitMB(
    boost::optional<int> jsHeapLimitMB) {
    params.jsHeapLimitMB = std::move(jsHeapLimitMB);
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::timeZoneDatabase(
    const TimeZoneDatabase* timeZoneDatabase) {
    params.timeZoneDatabase = timeZoneDatabase;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::changeStreamTokenVersion(
    int changeStreamTokenVersion) {
    params.changeStreamTokenVersion = changeStreamTokenVersion;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::changeStreamSpec(
    boost::optional<DocumentSourceChangeStreamSpec> changeStreamSpec) {
    params.changeStreamSpec = std::move(changeStreamSpec);
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::originalAggregateCommand(
    BSONObj originalAggregateCommand) {
    params.originalAggregateCommand = std::move(originalAggregateCommand);
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::initialPostBatchResumeToken(
    BSONObj initialPostBatchResumeToken) {
    params.initialPostBatchResumeToken = std::move(initialPostBatchResumeToken);
    return *this;
}

std::ostream& operator<<(std::ostream& os, SbeCompatibility sbeCompat) {
    switch (sbeCompat) {
#define CASE_SBE_COMPAT_OSTREAM(sbeCompat) \
    case sbeCompat: {                      \
        os << #sbeCompat;                  \
        return os;                         \
    }
        CASE_SBE_COMPAT_OSTREAM(SbeCompatibility::notCompatible)
        CASE_SBE_COMPAT_OSTREAM(SbeCompatibility::requiresSbeFull)
        CASE_SBE_COMPAT_OSTREAM(SbeCompatibility::requiresTrySbe)
        CASE_SBE_COMPAT_OSTREAM(SbeCompatibility::noRequirements)
#undef CASE_SBE_COMPAT_OSTREAM
    }
    tasserted(10230203, "missing case in 'operator<<' for 'SbeCompatibility'");
    return os;
}

ExpressionContextBuilder& ExpressionContextBuilder::sbeCompatibility(
    SbeCompatibility sbeCompatibility) {
    params.sbeCompatibility = sbeCompatibility;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::sbeGroupCompatibility(
    SbeCompatibility sbeGroupCompatibility) {
    params.sbeGroupCompatibility = sbeGroupCompatibility;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::sbeWindowCompatibility(
    SbeCompatibility sbeWindowCompatibility) {
    params.sbeWindowCompatibility = sbeWindowCompatibility;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::sbePipelineCompatibility(
    SbeCompatibility sbePipelineCompatibility) {
    params.sbePipelineCompatibility = sbePipelineCompatibility;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::collationMatchesDefault(
    ExpressionContextCollationMatchesDefault collationMatchesDefault) {
    params.collationMatchesDefault = collationMatchesDefault;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::serverSideJsConfig(
    const ExpressionContext::ServerSideJsConfig& serverSideJsConfig) {
    params.serverSideJsConfig = serverSideJsConfig;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::view(
    boost::optional<std::pair<NamespaceString, std::vector<BSONObj>>> view) {
    params.view = std::move(view);
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::originalNs(NamespaceString originalNs) {
    params.originalNs = std::move(originalNs);
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::isHybridSearch(bool isHybridSearch) {
    params.isHybridSearch = isHybridSearch;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::withReplicationResolvedNamespaces() {
    ResolvedNamespaceMap resolvedNamespaces;

    resolvedNamespaces[NamespaceString::kSessionTransactionsTableNamespace] = {
        NamespaceString::kSessionTransactionsTableNamespace, std::vector<BSONObj>()};

    resolvedNamespaces[NamespaceString::kRsOplogNamespace] = {NamespaceString::kRsOplogNamespace,
                                                              std::vector<BSONObj>()};

    resolvedNamespace(std::move(resolvedNamespaces));
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::subPipelineDepth(long long subPipelineDepth) {
    params.subPipelineDepth = subPipelineDepth;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::tailableMode(TailableModeEnum tailableMode) {
    params.tailableMode = tailableMode;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::fromRequest(
    OperationContext* operationContext,
    const FindCommandRequest& request,
    const CollatorInterface* collectionCollator,
    bool useDisk) {

    opCtx(operationContext);
    mayDbProfile(CurOp::get(operationContext)->dbProfileLevel() > 0);
    allowDiskUse(request.getAllowDiskUse().value_or(useDisk));
    ns(request.getNamespaceOrUUID().isNamespaceString() ? request.getNamespaceOrUUID().nss()
                                                        : NamespaceString{});
    if (request.getNamespaceOrUUID().isUUID()) {
        collUUID(request.getNamespaceOrUUID().uuid());
    }
    runtimeConstants(request.getLegacyRuntimeConstants());
    letParameters(request.getLet());
    serializationContext(request.getSerializationContext());
    tailableMode(query_request_helper::getTailableMode(request));

    if (!request.getCollation().isEmpty()) {
        auto requestCollator =
            uassertStatusOK(CollatorFactoryInterface::get(operationContext->getServiceContext())
                                ->makeFromBSON(request.getCollation()));

        // If request collator equals to collection collator then check for IDHACK eligibility.
        const bool haveMatchingCollators =
            CollatorInterface::collatorsMatch(requestCollator.get(), collectionCollator);
        if (haveMatchingCollators) {
            isIdHackQuery(isIdHackEligibleQueryWithoutCollator(request));
        }

        collator(std::move(requestCollator));
    } else {
        if (collectionCollator) {
            collator(collectionCollator->clone());
        } else {
            // If there is no collection or request collator we call
            // isIdHackEligibleQueryWithoutCollator() in order to evaluate if 'request' is an
            // IDHACK query.
            isIdHackQuery(isIdHackEligibleQueryWithoutCollator(request));
        }
    }

    isFleQuery(request.getEncryptionInformation().has_value());
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::fromRequest(OperationContext* opCtx,
                                                                const FindCommandRequest& request,
                                                                bool dbProfile) {
    fromRequest(opCtx, request, nullptr);
    mayDbProfile(dbProfile);
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::fromRequest(
    OperationContext* operationContext,
    const DistinctCommandRequest& request,
    const CollatorInterface* collatorInterface) {
    opCtx(operationContext);
    serializationContext(request.getSerializationContext());
    mayDbProfile(CurOp::get(operationContext)->dbProfileLevel() > 0);
    if (auto collationObj = request.getCollation().get_value_or(BSONObj());
        !collationObj.isEmpty()) {
        collator(
            uassertStatusOK(CollatorFactoryInterface::get(operationContext->getServiceContext())
                                ->makeFromBSON(collationObj)));
    } else if (collatorInterface) {
        collator(collatorInterface->clone());
    }

    if (request.getNamespaceOrUUID().isUUID()) {
        collUUID(request.getNamespaceOrUUID().uuid());
    }
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::fromRequest(
    OperationContext* operationContext, const AggregateCommandRequest& request, bool useDisk) {
    opCtx(operationContext);
    fromRouter(aggregation_request_helper::getFromRouter(request));
    if (!request.getNeedsMerge() && request.getNeedsSortedMerge()) {
        tasserted(10372401,
                  "Encountered malformed AggregationCommandRequest: needsMerge = false and "
                  "needsSortedMerge = true are contradictory");
    }
    MergeType type{MergeType::noMerge};
    if (request.getNeedsSortedMerge()) {
        type = MergeType::sortedMerge;
    } else if (request.getNeedsMerge()) {
        type = MergeType::unsortedMerge;
    }
    mergeType(type);
    allowDiskUse(request.getAllowDiskUse().value_or(useDisk));
    bypassDocumentValidation(request.getBypassDocumentValidation().value_or(false));
    isMapReduceCommand(request.getIsMapReduceCommand());
    forPerShardCursor(request.getPassthroughToShard().has_value());
    ns(request.getNamespace());
    runtimeConstants(request.getLegacyRuntimeConstants());
    letParameters(request.getLet());
    serializationContext(request.getSerializationContext());
    isFleQuery(request.getEncryptionInformation().has_value());
    return *this;
}

boost::intrusive_ptr<ExpressionContext> ExpressionContextBuilder::build() {
    auto expCtx = boost::intrusive_ptr<ExpressionContext>(new ExpressionContext{std::move(params)});

    if (expCtx->_params.letParameters) {
        expCtx->variables.seedVariablesWithLetParameters(
            expCtx.get(), *expCtx->_params.letParameters, [](const Expression* expr) {
                return expression::getDependencies(expr).hasNoRequirements();
            });
    }

    return expCtx;
}

boost::intrusive_ptr<ExpressionContext> makeBlankExpressionContext(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nssOrUUID,
    boost::optional<BSONObj> shapifiedLet) {
    auto nss = nssOrUUID.isNamespaceString() ? nssOrUUID.nss() : NamespaceString{};
    return ExpressionContextBuilder{}
        .opCtx(opCtx)
        .ns(nss)
        .originalNs(nss)
        .letParameters(std::move(shapifiedLet))
        .blankExpressionContext(true)
        .build();
}

boost::intrusive_ptr<ExpressionContext> makeCopyFromExpressionContext(
    const boost::intrusive_ptr<ExpressionContext>& other,
    NamespaceString ns,
    boost::optional<UUID> uuid,
    boost::optional<std::unique_ptr<CollatorInterface>> updatedCollator,
    boost::optional<std::pair<NamespaceString, std::vector<BSONObj>>> view,
    boost::optional<NamespaceString> userNs) {
    auto collator = [&]() {
        if (updatedCollator) {
            return std::move(*updatedCollator);
        } else if (other->getCollator()) {
            return other->getCollator()->clone();
        } else {
            return std::unique_ptr<CollatorInterface>();
        }
    }();

    // Some of the properties of expression context are not cloned (e.g runtimeConstants,
    // letParameters, view). In case new fields need to be cloned, they will need to be added in the
    // builder and the proper setter called here.
    auto expCtx =
        ExpressionContextBuilder()
            .opCtx(other->getOperationContext(), other->getVersionContext())
            .ifrContext(other->getIfrContext())
            .collator(std::move(collator))
            .mongoProcessInterface(other->getMongoProcessInterface())
            // For stages like $lookup and $unionWith that have a $rankFusion as subpipeline, we
            // want to pass the namespace of the spec (aka the executionNs) to avoid running against
            // an incorrect namespace, e.g.
            // db.collA.aggregate([$unionWith: {coll: collB, pipeline: <rankFusionCollB>}]);
            .originalNs(userNs.value_or(ns))
            .ns(std::move(ns))
            .resolvedNamespace(other->getResolvedNamespaces())
            .mayDbProfile(other->getMayDbProfile())
            .fromRouter(other->getFromRouter())
            .mergeType(other->mergeType())
            .forPerShardCursor(other->getForPerShardCursor())
            .allowDiskUse(other->getAllowDiskUse())
            .bypassDocumentValidation(other->getBypassDocumentValidation())
            .collUUID(uuid)
            .explain(other->getExplain())
            .inRouter(other->getInRouter())
            .tmpDir(other->getTempDir())
            .serializationContext(other->getSerializationContext())
            .inLookup(other->getInLookup())
            .isParsingViewDefinition(other->getIsParsingViewDefinition())
            .exprUnstableForApiV1(other->getExprUnstableForApiV1())
            .exprDeprecatedForApiV1(other->getExprDeprecatedForApiV1())
            .jsHeapLimitMB(other->getJsHeapLimitMB())
            .changeStreamTokenVersion(other->getChangeStreamTokenVersion())
            .changeStreamSpec(other->getChangeStreamSpec())
            .originalAggregateCommand(other->getOriginalAggregateCommand())
            .subPipelineDepth(other->getSubPipelineDepth())
            .initialPostBatchResumeToken(other->getInitialPostBatchResumeToken().getOwned())
            .view(view)
            .requiresTimeseriesExtendedRangeSupport(
                other->getRequiresTimeseriesExtendedRangeSupport())
            .isHybridSearch(other->isHybridSearch())
            .build();

    if (other->getIgnoreCollator()) {
        expCtx->setIgnoreCollator();
    }

    expCtx->variables = other->variables;
    expCtx->variablesParseState =
        other->variablesParseState.copyWith(expCtx->variables.useIdGenerator());

    expCtx->setQuerySettings(other->getOptionalQuerySettings());

    // Note that we intentionally skip copying the state of '_interruptChecker' because 'expCtx' is
    // intended to be used for executing a separate aggregation pipeline.

    return expCtx;
}

/**
 * Returns an ExpressionContext that is identical to 'this' except for the 'subPipelineDepth'
 * and 'needsMerge' fields, as well as changing the 'userNs' to whatever was passed in as
 * 'from' in $lookup or 'coll' in $unionWith.
 */
boost::intrusive_ptr<ExpressionContext> makeCopyForSubPipelineFromExpressionContext(
    const boost::intrusive_ptr<ExpressionContext>& other,
    NamespaceString nss,
    boost::optional<UUID> uuid,
    boost::optional<NamespaceString> userNs) {
    uassert(ErrorCodes::MaxSubPipelineDepthExceeded,
            str::stream() << "Maximum number of nested sub-pipelines exceeded. Limit is "
                          << internalMaxSubPipelineViewDepth.load(),
            other->getSubPipelineDepth() < internalMaxSubPipelineViewDepth.load());
    // Initialize any referenced system variables now so that the subpipeline has the same
    // values for these variables.
    other->initializeReferencedSystemVariables();
    auto newCopy = makeCopyFromExpressionContext(
        other, std::move(nss), uuid, boost::none, boost::none, userNs);
    newCopy->setSubPipelineDepth(newCopy->getSubPipelineDepth() + 1);
    // The original expCtx might have been attached to an aggregation pipeline running on the
    // shards. We must reset 'needsMerge' in order to get fully merged results for the
    // subpipeline.
    newCopy->setNeedsMerge(false);
    return newCopy;
}

}  // namespace mongo
