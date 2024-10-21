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
#include "mongo/db/feature_compatibility_version_documentation.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/stats/counters.h"

namespace mongo {

ExpressionContextBuilder& ExpressionContextBuilder::opCtx(OperationContext* optCtx) {
    params.opCtx = optCtx;
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
    StringMap<ResolvedNamespace> resolvedNamespaces) {
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

ExpressionContextBuilder& ExpressionContextBuilder::blankExpressionContext(
    bool blankExpressionContext) {
    params.blankExpressionContext = blankExpressionContext;
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

ExpressionContextBuilder& ExpressionContextBuilder::collationMatchesDefault(
    ExpressionContextCollationMatchesDefault collationMatchesDefault) {
    params.collationMatchesDefault = collationMatchesDefault;
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::withReplicationResolvedNamespaces() {
    StringMap<ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces[NamespaceString::kTenantMigrationOplogView.coll()] = {
        NamespaceString::kTenantMigrationOplogView, std::vector<BSONObj>()};

    resolvedNamespaces[NamespaceString::kSessionTransactionsTableNamespace.coll()] = {
        NamespaceString::kSessionTransactionsTableNamespace, std::vector<BSONObj>()};

    resolvedNamespaces[NamespaceString::kRsOplogNamespace.coll()] = {
        NamespaceString::kRsOplogNamespace, std::vector<BSONObj>()};

    resolvedNamespace(std::move(resolvedNamespaces));
    return *this;
}

ExpressionContextBuilder& ExpressionContextBuilder::fromRequest(
    OperationContext* operationContext,
    const FindCommandRequest& request,
    const CollatorInterface* collatorInterface,
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
        collator(
            uassertStatusOK(CollatorFactoryInterface::get(operationContext->getServiceContext())
                                ->makeFromBSON(request.getCollation())));
    } else if (collatorInterface) {
        collator(collatorInterface->clone());
    }

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
    explain(request.getExplain());
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
    return *this;
}

boost::intrusive_ptr<ExpressionContext> ExpressionContextBuilder::build() {
    return boost::intrusive_ptr<ExpressionContext>(new ExpressionContext{std::move(params)});
}

ResolvedNamespace::ResolvedNamespace(NamespaceString ns,
                                     std::vector<BSONObj> pipeline,
                                     boost::optional<UUID> collUUID)
    : ns(std::move(ns)), pipeline(std::move(pipeline)), uuid(collUUID) {}

ExpressionContext::ExpressionContext(ExpressionContextParams&& params)
    : explain(params.explain),
      fromRouter(params.fromRouter),
      needsMerge(params.needsMerge),
      inRouter(params.inRouter),
      forPerShardCursor(params.forPerShardCursor),
      allowDiskUse(params.allowDiskUse && ([&]() {
                       tassert(7738401, "opCtx null check", params.opCtx);
                       return !(params.opCtx->readOnly());
                   }())),  // Disallow disk use if in read-only mode.
      bypassDocumentValidation(params.bypassDocumentValidation),
      hasWhereClause(params.hasWhereClause),
      isUpsert(params.isUpsert),
      ns(params.ns),
      serializationCtxt(params.serializationContext),
      uuid(std::move(params.collUUID)),
      tempDir(std::move(params.tmpDir)),
      opCtx(params.opCtx),
      mongoProcessInterface(params.mongoProcessInterface
                                ? params.mongoProcessInterface
                                : std::make_shared<StubMongoProcessInterface>()),
      timeZoneDatabase(getTimeZoneDatabase(opCtx)),
      variablesParseState(variables.useIdGenerator()),
      mayDbProfile(params.mayDbProfile),
      collationMatchesDefault(params.collationMatchesDefault),
      _collator(std::move(params.collator)),
      _documentComparator(_collator.getCollator()),
      _valueComparator(_collator.getCollator()),
      _resolvedNamespaces(std::move(params.resolvedNamespaces)) {

    // Only initialize 'variables' if we are given a runtimeConstants object. We delay initializing
    // variables and expect callers to invoke 'initializeReferencedSystemVariables()' after query
    // parsing. This allows us to only initialize variables which are used in the query.

    if (params.blankExpressionContext) {
        // This is a shortcut to avoid reading the clock and the vector clock, since we don't
        // actually care about their values for this 'blank' ExpressionContext codepath.
        variables.setLegacyRuntimeConstants({Date_t::min(), Timestamp()});
        // Expression counters are reported in serverStatus to indicate how often clients use
        // certain expressions/stages, so it's a side effect tied to parsing. We must stop
        // expression counters before re-parsing to avoid adding to the counters more than once per
        // a given query.
        stopExpressionCounters();
    } else if (params.runtimeConstants) {
        if (params.runtimeConstants->getClusterTime().isNull()) {
            // Try to get a default value for clusterTime if a logical clock exists.
            auto genConsts = variables.generateRuntimeConstants(opCtx);
            genConsts.setJsScope(params.runtimeConstants->getJsScope());
            genConsts.setIsMapReduce(params.runtimeConstants->getIsMapReduce());
            genConsts.setUserRoles(params.runtimeConstants->getUserRoles());
            variables.setLegacyRuntimeConstants(genConsts);
        } else {
            variables.setLegacyRuntimeConstants(*(params.runtimeConstants));
        }
    }
    if (params.letParameters) {
        variables.seedVariablesWithLetParameters(this, *params.letParameters);
    }
    if (!params.isMapReduceCommand) {
        jsHeapLimitMB = internalQueryJavaScriptHeapSizeLimitMB.load();
    } else {
        jsHeapLimitMB = boost::none;
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
        .letParameters(shapifiedLet)
        .blankExpressionContext(true)
        .build();
}

void ExpressionContext::checkForInterruptSlow() {
    // This check could be expensive, at least in relative terms, so don't check every time.
    invariant(opCtx);
    _interruptCounter = kInterruptCheckPeriod;
    opCtx->checkForInterrupt();
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
    boost::optional<std::unique_ptr<CollatorInterface>> updatedCollator) const {
    auto collator = [&]() {
        if (updatedCollator) {
            return std::move(*updatedCollator);
        } else if (_collator.getCollator()) {
            return _collator.getCollator()->clone();
        } else {
            return std::unique_ptr<CollatorInterface>();
        }
    }();

    auto expCtx = ExpressionContextBuilder()
                      .opCtx(opCtx)
                      .collator(std::move(collator))
                      .mongoProcessInterface(mongoProcessInterface)
                      .ns(ns)
                      .resolvedNamespace(_resolvedNamespaces)
                      .mayDbProfile(mayDbProfile)
                      .fromRouter(fromRouter)
                      .needsMerge(needsMerge)
                      .forPerShardCursor(forPerShardCursor)
                      .allowDiskUse(allowDiskUse)
                      .bypassDocumentValidation(bypassDocumentValidation)
                      .collUUID(uuid)
                      .explain(explain)
                      .build();

    if (_collator.getIgnore()) {
        expCtx->setIgnoreCollator();
    }

    expCtx->inRouter = inRouter;
    expCtx->maxFeatureCompatibilityVersion = maxFeatureCompatibilityVersion;
    expCtx->subPipelineDepth = subPipelineDepth;
    expCtx->tempDir = tempDir;
    expCtx->jsHeapLimitMB = jsHeapLimitMB;
    expCtx->isParsingViewDefinition = isParsingViewDefinition;

    expCtx->variables = variables;
    expCtx->variablesParseState = variablesParseState.copyWith(expCtx->variables.useIdGenerator());
    expCtx->exprUnstableForApiV1 = exprUnstableForApiV1;
    expCtx->exprDeprectedForApiV1 = exprDeprectedForApiV1;

    expCtx->initialPostBatchResumeToken = initialPostBatchResumeToken.getOwned();
    expCtx->changeStreamTokenVersion = changeStreamTokenVersion;
    expCtx->changeStreamSpec = changeStreamSpec;

    expCtx->originalAggregateCommand = originalAggregateCommand.getOwned();

    expCtx->inLookup = inLookup;
    expCtx->serializationCtxt = serializationCtxt;

    expCtx->_querySettings = _querySettings;

    // Note that we intentionally skip copying the value of '_interruptCounter' because 'expCtx' is
    // intended to be used for executing a separate aggregation pipeline.

    return expCtx;
}

void ExpressionContext::startExpressionCounters() {
    if (enabledCounters && !_expressionCounters) {
        _expressionCounters = std::make_unique<ExpressionCounters>();
    }
}

void ExpressionContext::incrementMatchExprCounter(StringData name) {
    if (enabledCounters && _expressionCounters) {
        ++_expressionCounters->matchExprCountersMap[name];
    }
}

void ExpressionContext::incrementAggExprCounter(StringData name) {
    if (enabledCounters && _expressionCounters) {
        ++_expressionCounters->aggExprCountersMap[name];
    }
}

void ExpressionContext::incrementGroupAccumulatorExprCounter(StringData name) {
    if (enabledCounters && _expressionCounters) {
        ++_expressionCounters->groupAccumulatorExprCountersMap[name];
    }
}

void ExpressionContext::incrementWindowAccumulatorExprCounter(StringData name) {
    if (enabledCounters && _expressionCounters) {
        ++_expressionCounters->windowAccumulatorExprCountersMap[name];
    }
}

void ExpressionContext::stopExpressionCounters() {
    if (enabledCounters && _expressionCounters) {
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
        variables.defineClusterTime(opCtx);
    }
    if (_systemVarsReferencedInQuery.contains(Variables::kUserRolesId) &&
        !variables.hasValue(Variables::kUserRolesId) && enableAccessToUserRoles.load()) {
        variables.defineUserRoles(opCtx);
    }
}

void ExpressionContext::throwIfFeatureFlagIsNotEnabledOnFCV(
    StringData name, const boost::optional<FeatureFlag>& flag) {
    if (!flag) {
        return;
    }

    // If the FCV is not initialized yet, we check whether the feature flag is enabled on the last
    // LTS FCV, which is the lowest FCV we can have on this server. If the FCV is set, then we
    // should check if the flag is enabled on maxFeatureCompatibilityVersion or the current FCV. If
    // both the FCV is uninitialized and maxFeatureCompatibilityVersion is set, to be safe, we
    // should check the lowest FCV. We are guaranteed that maxFeatureCompatibilityVersion will
    // always be greater than or equal to the last LTS. So we will check the last LTS.
    const auto fcv = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    mongo::multiversion::FeatureCompatibilityVersion versionToCheck = fcv.getVersion();
    if (!fcv.isVersionInitialized()) {
        // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
        versionToCheck = multiversion::GenericFCV::kLastLTS;
    } else if (maxFeatureCompatibilityVersion) {
        versionToCheck = *maxFeatureCompatibilityVersion;
    }

    uassert(ErrorCodes::QueryFeatureNotAllowed,
            // We would like to include the current version and the required minimum version in this
            // error message, but using FeatureCompatibilityVersion::toString() would introduce a
            // dependency cycle (see SERVER-31968).
            str::stream() << name
                          << " is not allowed in the current feature compatibility version. See "
                          << feature_compatibility_version_documentation::kCompatibilityLink
                          << " for more information.",
            flag->isEnabledOnVersion(versionToCheck));
}

}  // namespace mongo
