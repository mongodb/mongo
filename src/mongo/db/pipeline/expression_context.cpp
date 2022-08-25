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

#include <utility>

#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

ExpressionContext::ResolvedNamespace::ResolvedNamespace(NamespaceString ns,
                                                        std::vector<BSONObj> pipeline,
                                                        boost::optional<UUID> collUUID)
    : ns(std::move(ns)), pipeline(std::move(pipeline)), uuid(collUUID) {}

ExpressionContext::ExpressionContext(OperationContext* opCtx,
                                     const AggregateCommandRequest& request,
                                     std::unique_ptr<CollatorInterface> collator,
                                     std::shared_ptr<MongoProcessInterface> processInterface,
                                     StringMap<ResolvedNamespace> resolvedNamespaces,
                                     boost::optional<UUID> collUUID,
                                     bool mayDbProfile)
    : ExpressionContext(opCtx,
                        request.getExplain(),
                        request.getFromMongos(),
                        request.getNeedsMerge(),
                        request.getAllowDiskUse(),
                        request.getBypassDocumentValidation().value_or(false),
                        request.getIsMapReduceCommand(),
                        request.getNamespace(),
                        request.getLegacyRuntimeConstants(),
                        std::move(collator),
                        std::move(processInterface),
                        std::move(resolvedNamespaces),
                        std::move(collUUID),
                        request.getLet(),
                        mayDbProfile) {

    if (request.getIsMapReduceCommand()) {
        // mapReduce command JavaScript invocation is only subject to the server global
        // 'jsHeapLimitMB' limit.
        jsHeapLimitMB = boost::none;
    }
}

ExpressionContext::ExpressionContext(
    OperationContext* opCtx,
    const boost::optional<ExplainOptions::Verbosity>& explain,
    bool fromMongos,
    bool needsMerge,
    bool allowDiskUse,
    bool bypassDocumentValidation,
    bool isMapReduce,
    const NamespaceString& ns,
    const boost::optional<LegacyRuntimeConstants>& runtimeConstants,
    std::unique_ptr<CollatorInterface> collator,
    const std::shared_ptr<MongoProcessInterface>& mongoProcessInterface,
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces,
    boost::optional<UUID> collUUID,
    const boost::optional<BSONObj>& letParameters,
    bool mayDbProfile)
    : explain(explain),
      fromMongos(fromMongos),
      needsMerge(needsMerge),
      allowDiskUse(allowDiskUse),
      bypassDocumentValidation(bypassDocumentValidation),
      ns(ns),
      uuid(std::move(collUUID)),
      opCtx(opCtx),
      mongoProcessInterface(mongoProcessInterface),
      timeZoneDatabase(getTimeZoneDatabase(opCtx)),
      variablesParseState(variables.useIdGenerator()),
      mayDbProfile(mayDbProfile),
      _collator(std::move(collator)),
      _documentComparator(_collator.get()),
      _valueComparator(_collator.get()),
      _resolvedNamespaces(std::move(resolvedNamespaces)) {

    if (runtimeConstants && runtimeConstants->getClusterTime().isNull()) {
        // Try to get a default value for clusterTime if a logical clock exists.
        auto genConsts = variables.generateRuntimeConstants(opCtx);
        genConsts.setJsScope(runtimeConstants->getJsScope());
        genConsts.setIsMapReduce(runtimeConstants->getIsMapReduce());
        variables.setLegacyRuntimeConstants(genConsts);
    } else if (runtimeConstants) {
        variables.setLegacyRuntimeConstants(*runtimeConstants);
    } else {
        variables.setDefaultRuntimeConstants(opCtx);
    }

    if (!isMapReduce) {
        jsHeapLimitMB = internalQueryJavaScriptHeapSizeLimitMB.load();
    }
    if (letParameters)
        variables.seedVariablesWithLetParameters(this, *letParameters);
}

ExpressionContext::ExpressionContext(
    OperationContext* opCtx,
    std::unique_ptr<CollatorInterface> collator,
    const NamespaceString& nss,
    const boost::optional<LegacyRuntimeConstants>& runtimeConstants,
    const boost::optional<BSONObj>& letParameters,
    bool mayDbProfile,
    boost::optional<ExplainOptions::Verbosity> explain)
    : explain(explain),
      ns(nss),
      opCtx(opCtx),
      mongoProcessInterface(std::make_shared<StubMongoProcessInterface>()),
      timeZoneDatabase(opCtx && opCtx->getServiceContext()
                           ? TimeZoneDatabase::get(opCtx->getServiceContext())
                           : nullptr),
      variablesParseState(variables.useIdGenerator()),
      mayDbProfile(mayDbProfile),
      _collator(std::move(collator)),
      _documentComparator(_collator.get()),
      _valueComparator(_collator.get()) {
    if (runtimeConstants) {
        variables.setLegacyRuntimeConstants(*runtimeConstants);
    }

    jsHeapLimitMB = internalQueryJavaScriptHeapSizeLimitMB.load();
    if (letParameters)
        variables.seedVariablesWithLetParameters(this, *letParameters);
}

void ExpressionContext::checkForInterruptSlow() {
    // This check could be expensive, at least in relative terms, so don't check every time.
    invariant(opCtx);
    _interruptCounter = kInterruptCheckPeriod;
    opCtx->checkForInterrupt();
}

ExpressionContext::CollatorStash::CollatorStash(ExpressionContext* const expCtx,
                                                std::unique_ptr<CollatorInterface> newCollator)
    : _expCtx(expCtx), _originalCollator(std::move(_expCtx->_collator)) {
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

    auto collator = updatedCollator
        ? std::move(*updatedCollator)
        : (_collator ? _collator->clone() : std::unique_ptr<CollatorInterface>{});

    auto expCtx = make_intrusive<ExpressionContext>(opCtx,
                                                    explain,
                                                    fromMongos,
                                                    needsMerge,
                                                    allowDiskUse,
                                                    bypassDocumentValidation,
                                                    false,  // isMapReduce
                                                    ns,
                                                    boost::none,  // runtimeConstants
                                                    std::move(collator),
                                                    mongoProcessInterface,
                                                    _resolvedNamespaces,
                                                    uuid,
                                                    boost::none /* letParameters */,
                                                    mayDbProfile);

    expCtx->inMongos = inMongos;
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

    // Note that we intentionally skip copying the value of '_interruptCounter' because 'expCtx' is
    // intended to be used for executing a separate aggregation pipeline.

    return expCtx;
}

void ExpressionContext::startExpressionCounters() {
    if (enabledCounters && !_expressionCounters) {
        _expressionCounters = boost::make_optional<ExpressionCounters>({});
    }
}

void ExpressionContext::incrementMatchExprCounter(StringData name) {
    if (enabledCounters && _expressionCounters) {
        ++_expressionCounters.value().matchExprCountersMap[name];
    }
}

void ExpressionContext::incrementAggExprCounter(StringData name) {
    if (enabledCounters && _expressionCounters) {
        ++_expressionCounters.value().aggExprCountersMap[name];
    }
}

void ExpressionContext::incrementGroupAccumulatorExprCounter(StringData name) {
    if (enabledCounters && _expressionCounters) {
        ++_expressionCounters.value().groupAccumulatorExprCountersMap[name];
    }
}

void ExpressionContext::incrementWindowAccumulatorExprCounter(StringData name) {
    if (enabledCounters && _expressionCounters) {
        ++_expressionCounters.value().windowAccumulatorExprCountersMap[name];
    }
}

void ExpressionContext::stopExpressionCounters() {
    if (enabledCounters && _expressionCounters) {
        operatorCountersMatchExpressions.mergeCounters(
            _expressionCounters.value().matchExprCountersMap);
        operatorCountersAggExpressions.mergeCounters(
            _expressionCounters.value().aggExprCountersMap);
        operatorCountersGroupAccumulatorExpressions.mergeCounters(
            _expressionCounters.value().groupAccumulatorExprCountersMap);
        operatorCountersWindowAccumulatorExpressions.mergeCounters(
            _expressionCounters.value().windowAccumulatorExprCountersMap);
    }
    _expressionCounters = boost::none;
}

}  // namespace mongo
