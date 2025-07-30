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
#include "mongo/db/pipeline/expression_context.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/feature_compatibility_version_documentation.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/version_context.h"

#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

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

    if (!_params.isMapReduceCommand) {
        _params.jsHeapLimitMB = internalQueryJavaScriptHeapSizeLimitMB.load();
    } else {
        _params.jsHeapLimitMB = boost::none;
    }
    if (!_params.mongoProcessInterface) {
        _params.mongoProcessInterface = std::make_shared<StubMongoProcessInterface>();
    }
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
