/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <utility>

#include "mongo/bson/timestamp.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/feature_compatibility_version_documentation.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/version_context.h"

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

ExpressionContextBuilder& ExpressionContextBuilder::tmpDir(std::string tmpDir) {
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

ExpressionContextBuilder& ExpressionContextBuilder::needsMerge(bool needsMerge) {
    params.needsMerge = needsMerge;
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
    needsMerge(request.getNeedsMerge());
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
    return boost::intrusive_ptr<ExpressionContext>(new ExpressionContext{std::move(params)});
}

ResolvedNamespace::ResolvedNamespace(NamespaceString ns,
                                     std::vector<BSONObj> pipeline,
                                     boost::optional<UUID> collUUID,
                                     bool involvedNamespaceIsAView)
    : ns(std::move(ns)),
      pipeline(std::move(pipeline)),
      uuid(collUUID),
      involvedNamespaceIsAView(involvedNamespaceIsAView) {}

ExpressionContext::ExpressionContext(ExpressionContextParams&& params)
    : variablesParseState(variables.useIdGenerator()),
      _params(std::move(params)),
      _collator(std::move(_params.collator)),
      _documentComparator(_collator.getCollator()),
      _valueComparator(_collator.getCollator()),
      _interruptChecker(this),
      _featureFlagStreams([](const VersionContext& vCtx) {
          return gFeatureFlagStreams.isEnabledUseLastLTSFCVWhenUninitialized(
              vCtx, serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
      }) {

    _params.timeZoneDatabase = mongo::getTimeZoneDatabase(_params.opCtx);

    // Disallow disk use if in read-only mode.
    if (_params.allowDiskUse) {
        tassert(7738401, "opCtx null check", _params.opCtx);
        _params.allowDiskUse &= !(_params.opCtx->readOnly());
    }

    // Only initialize 'variables' if we are given a runtimeConstants object. We delay initializing
    // variables and expect callers to invoke 'initializeReferencedSystemVariables()' after query
    // parsing. This allows us to only initialize variables which are used in the query.

    if (_params.blankExpressionContext) {
        // This is a shortcut to avoid reading the clock and the vector clock, since we don't
        // actually care about their values for this 'blank' ExpressionContext codepath.
        variables.setLegacyRuntimeConstants({Date_t::min(), Timestamp()});
        // Expression counters are reported in serverStatus to indicate how often clients use
        // certain expressions/stages, so it's a side effect tied to parsing. We must stop
        // expression counters before re-parsing to avoid adding to the counters more than once per
        // a given query.
        stopExpressionCounters();
    } else if (_params.runtimeConstants) {
        if (_params.runtimeConstants->getClusterTime().isNull()) {
            // Try to get a default value for clusterTime if a logical clock exists.
            auto genConsts = variables.generateRuntimeConstants(getOperationContext());
            genConsts.setJsScope(_params.runtimeConstants->getJsScope());
            genConsts.setIsMapReduce(_params.runtimeConstants->getIsMapReduce());
            genConsts.setUserRoles(_params.runtimeConstants->getUserRoles());
            variables.setLegacyRuntimeConstants(genConsts);
        } else {
            variables.setLegacyRuntimeConstants(*(_params.runtimeConstants));
        }
    }
    if (_params.letParameters) {
        variables.seedVariablesWithLetParameters(this, *_params.letParameters);
    }
    if (!_params.isMapReduceCommand) {
        _params.jsHeapLimitMB = internalQueryJavaScriptHeapSizeLimitMB.load();
    } else {
        _params.jsHeapLimitMB = boost::none;
    }
    if (!_params.mongoProcessInterface) {
        _params.mongoProcessInterface = std::make_shared<StubMongoProcessInterface>();
    }
}

boost::intrusive_ptr<ExpressionContext> ExpressionContext::makeBlankExpressionContext(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nssOrUUID,
    boost::optional<BSONObj> shapifiedLet) {
    auto nss = nssOrUUID.isNamespaceString() ? nssOrUUID.nss() : NamespaceString{};
    return ExpressionContextBuilder{}
        .opCtx(opCtx)
        .ns(nss)
        .letParameters(std::move(shapifiedLet))
        .blankExpressionContext(true)
        .build();
}

ExpressionContext::CollatorStash::CollatorStash(ExpressionContext* const expCtx,
                                                std::unique_ptr<CollatorInterface> newCollator)
    : _expCtx(expCtx), _originalCollator(_expCtx->_collator.getCollatorShared()) {
    _expCtx->setCollator(std::move(newCollator));
}

ExpressionContext::CollatorStash::~CollatorStash() {
    _expCtx->setCollator(std::move(_originalCollator));
}

std::unique_ptr<ExpressionContext::CollatorStash> ExpressionContext::temporarilyChangeCollator(
    std::unique_ptr<CollatorInterface> newCollator) {
    // This constructor of CollatorStash is private, so we can't use make_unique().
    return std::unique_ptr<CollatorStash>(new CollatorStash(this, std::move(newCollator)));
}

boost::intrusive_ptr<ExpressionContext> ExpressionContext::copyWith(
    NamespaceString ns,
    boost::optional<UUID> uuid,
    boost::optional<std::unique_ptr<CollatorInterface>> updatedCollator,
    boost::optional<std::pair<NamespaceString, std::vector<BSONObj>>> view) const {
    auto collator = [&]() {
        if (updatedCollator) {
            return std::move(*updatedCollator);
        } else if (_collator.getCollator()) {
            return _collator.getCollator()->clone();
        } else {
            return std::unique_ptr<CollatorInterface>();
        }
    }();

    // Some of the properties of expression context are not cloned (e.g runtimeConstants,
    // letParameters, view). In case new fields need to be cloned, they will need to be added in the
    // builder and the proper setter called here.
    auto expCtx = ExpressionContextBuilder()
                      .opCtx(_params.opCtx, _params.vCtx)
                      .ifrContext(_params.ifrContext)
                      .collator(std::move(collator))
                      .mongoProcessInterface(_params.mongoProcessInterface)
                      .ns(std::move(ns))
                      .resolvedNamespace(_params.resolvedNamespaces)
                      .mayDbProfile(_params.mayDbProfile)
                      .fromRouter(_params.fromRouter)
                      .needsMerge(_params.needsMerge)
                      .forPerShardCursor(_params.forPerShardCursor)
                      .allowDiskUse(_params.allowDiskUse)
                      .bypassDocumentValidation(_params.bypassDocumentValidation)
                      .collUUID(uuid)
                      .explain(_params.explain)
                      .inRouter(_params.inRouter)
                      .tmpDir(_params.tmpDir)
                      .serializationContext(_params.serializationContext)
                      .inLookup(_params.inLookup)
                      .isParsingViewDefinition(_params.isParsingViewDefinition)
                      .exprUnstableForApiV1(_params.exprUnstableForApiV1)
                      .exprDeprecatedForApiV1(_params.exprDeprecatedForApiV1)
                      .jsHeapLimitMB(_params.jsHeapLimitMB)
                      .changeStreamTokenVersion(_params.changeStreamTokenVersion)
                      .changeStreamSpec(_params.changeStreamSpec)
                      .originalAggregateCommand(_params.originalAggregateCommand)
                      .subPipelineDepth(_params.subPipelineDepth)
                      .initialPostBatchResumeToken(_params.initialPostBatchResumeToken.getOwned())
                      .view(view)
                      .build();

    if (_collator.getIgnore()) {
        expCtx->setIgnoreCollator();
    }

    expCtx->variables = variables;
    expCtx->variablesParseState = variablesParseState.copyWith(expCtx->variables.useIdGenerator());

    expCtx->_querySettings = _querySettings;

    // Note that we intentionally skip copying the state of '_interruptChecker' because 'expCtx' is
    // intended to be used for executing a separate aggregation pipeline.

    return expCtx;
}

void ExpressionContext::startExpressionCounters() {
    if (_params.enabledCounters && !_expressionCounters) {
        _expressionCounters = std::make_unique<ExpressionCounters>();
    }
}

void ExpressionContext::incrementMatchExprCounter(StringData name) {
    if (_params.enabledCounters && _expressionCounters) {
        ++_expressionCounters->matchExprCountersMap[name];
    }
}

void ExpressionContext::incrementAggExprCounter(StringData name) {
    if (_params.enabledCounters && _expressionCounters) {
        ++_expressionCounters->aggExprCountersMap[name];
    }
}

void ExpressionContext::incrementGroupAccumulatorExprCounter(StringData name) {
    if (_params.enabledCounters && _expressionCounters) {
        ++_expressionCounters->groupAccumulatorExprCountersMap[name];
    }
}

void ExpressionContext::incrementWindowAccumulatorExprCounter(StringData name) {
    if (_params.enabledCounters && _expressionCounters) {
        ++_expressionCounters->windowAccumulatorExprCountersMap[name];
    }
}

void ExpressionContext::stopExpressionCounters() {
    if (_params.enabledCounters && _expressionCounters) {
        operatorCountersMatchExpressions.mergeCounters(_expressionCounters->matchExprCountersMap);
        operatorCountersAggExpressions.mergeCounters(_expressionCounters->aggExprCountersMap);
        operatorCountersGroupAccumulatorExpressions.mergeCounters(
            _expressionCounters->groupAccumulatorExprCountersMap);
        operatorCountersWindowAccumulatorExpressions.mergeCounters(
            _expressionCounters->windowAccumulatorExprCountersMap);
    }
    _expressionCounters.reset();
}

void ExpressionContext::initializeReferencedSystemVariables() {
    if (_systemVarsReferencedInQuery.contains(Variables::kNowId) &&
        !variables.hasValue(Variables::kNowId)) {
        variables.defineLocalNow();
    }
    if (_systemVarsReferencedInQuery.contains(Variables::kClusterTimeId) &&
        !variables.hasValue(Variables::kClusterTimeId)) {
        // TODO: SERVER-104560: Create utility functions that return the current server topology.
        // Replace the code checking for standalone mode with a call to the utility provided by
        // SERVER-104560.
        auto* repl = repl::ReplicationCoordinator::get(getOperationContext());
        if (repl && !repl->getSettings().isReplSet() &&
            serverGlobalParams.clusterRole.has(ClusterRole::None)) {
            uasserted(10071200,
                      str::stream() << "system variable $$CLUSTER_TIME"
                                    << " is not available in standalone mode");
        }
        variables.defineClusterTime(getOperationContext());
    }
    if (_systemVarsReferencedInQuery.contains(Variables::kUserRolesId) &&
        !variables.hasValue(Variables::kUserRolesId) && enableAccessToUserRoles.load()) {
        variables.defineUserRoles(getOperationContext());
    }
}

void ExpressionContext::throwIfParserShouldRejectFeature(StringData name, FeatureFlag& flag) {
    // (Generic FCV reference): Fall back to kLastLTS when 'vCtx' is not initialized.
    uassert(
        ErrorCodes::QueryFeatureNotAllowed,
        str::stream() << name
                      << " is not allowed in the current feature compatibility version. See "
                      << feature_compatibility_version_documentation::compatibilityLink()
                      << " for more information.",
        flag.checkWithContext(_params.vCtx,
                              _params.ifrContext,
                              ServerGlobalParams::FCVSnapshot{multiversion::GenericFCV::kLastLTS}));
}

void ExpressionContext::ignoreFeatureInParserOrRejectAndThrow(StringData name, FeatureFlag& flag) {
    if (!shouldParserIgnoreFeatureFlagCheck()) {
        throwIfParserShouldRejectFeature(name, flag);
    }
}

}  // namespace mongo
