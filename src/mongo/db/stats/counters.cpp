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

#include "mongo/db/stats/counters.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/client/authenticate.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/util/static_immortal.h"

#include <tuple>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

using ::mongo::otel::metrics::CounterOptions;
using ::mongo::otel::metrics::MetricNames;
using ::mongo::otel::metrics::MetricsService;
using ::mongo::otel::metrics::MetricUnit;
using ::mongo::otel::metrics::ServerStatusOptions;

NetworkCounter::NetworkCounter()
    : _ingressLogicalBytesIn(MetricsService::instance().createInt64Counter(
          MetricNames::kNetworkIngressBytesIn,
          "Total number of logical bytes received from ingress (wire-protocol) clients.",
          MetricUnit::kBytes)),
      _ingressNumRequests(MetricsService::instance().createInt64Counter(
          MetricNames::kNetworkIngressNumRequests,
          "Total number of requests received from ingress (wire-protocol) clients.",
          MetricUnit::kOperations)),
      _ingressLogicalBytesOut(MetricsService::instance().createInt64Counter(
          MetricNames::kNetworkIngressBytesOut,
          "Total number of logical bytes sent to ingress (wire-protocol) clients.",
          MetricUnit::kBytes)),
      _egressLogicalBytesIn(MetricsService::instance().createInt64Counter(
          MetricNames::kNetworkEgressBytesIn,
          "Total number of logical bytes received on egress (outbound client) connections.",
          MetricUnit::kBytes)),
      _egressNumRequests(MetricsService::instance().createInt64Counter(
          MetricNames::kNetworkEgressNumRequests,
          "Total number of requests sent on egress (outbound client) connections.",
          MetricUnit::kOperations)),
      _egressLogicalBytesOut(MetricsService::instance().createInt64Counter(
          MetricNames::kNetworkEgressBytesOut,
          "Total number of logical bytes sent on egress (outbound client) connections.",
          MetricUnit::kBytes)),
      _numSlowDNSOperations(MetricsService::instance().createInt64Counter(
          MetricNames::kNetworkNumSlowDNSOperations,
          "Total number of slow DNS resolution operations.",
          MetricUnit::kCount)),
      _numSlowSSLOperations(MetricsService::instance().createInt64Counter(
          MetricNames::kNetworkNumSlowSSLOperations,
          "Total number of slow SSL handshake operations.",
          MetricUnit::kCount)) {}

void NetworkCounter::hitPhysicalIn(ConnectionType connectionType, long long bytes) {
    static const int64_t MAX = 1ULL << 60;
    auto& ref = connectionType == ConnectionType::kIngress ? _ingressPhysicalBytesIn
                                                           : _egressPhysicalBytesIn;

    // don't care about the race as its just a counter
    const bool overflow = ref->loadRelaxed() > MAX;

    if (overflow) {
        ref->store(bytes);
    } else {
        ref->fetchAndAdd(bytes);
    }
}

void NetworkCounter::hitPhysicalOut(ConnectionType connectionType, long long bytes) {
    static const int64_t MAX = 1ULL << 60;
    auto& ref = connectionType == ConnectionType::kIngress ? _ingressPhysicalBytesOut
                                                           : _egressPhysicalBytesOut;

    // don't care about the race as its just a counter
    const bool overflow = ref->loadRelaxed() > MAX;

    if (overflow) {
        ref->store(bytes);
    } else {
        ref->fetchAndAdd(bytes);
    }
}

void NetworkCounter::hitLogicalIn(ConnectionType connectionType, long long bytes) {
    // The requests field only gets incremented here (and not in hitPhysical) because
    // hitLogical and hitPhysical are each called for each operation. Incrementing it in both
    // functions would double-count the number of operations.
    if (connectionType == ConnectionType::kIngress) {
        _ingressLogicalBytesIn.add(bytes);
        _ingressNumRequests.add(1);
    } else {
        _egressLogicalBytesIn.add(bytes);
        _egressNumRequests.add(1);
    }
}

void NetworkCounter::hitLogicalOut(ConnectionType connectionType, long long bytes) {
    if (connectionType == ConnectionType::kIngress) {
        _ingressLogicalBytesOut.add(bytes);
    } else {
        _egressLogicalBytesOut.add(bytes);
    }
}

void NetworkCounter::incrementNumSlowDNSOperations() {
    _numSlowDNSOperations.add(1);
}

void NetworkCounter::incrementNumSlowSSLOperations() {
    _numSlowSSLOperations.add(1);
}

void NetworkCounter::acceptedTFOIngress() {
    _tfoAccepted->fetchAndAddRelaxed(1);
}

void NetworkCounter::append(BSONObjBuilder& b) {
    b.append("bytesIn", _ingressLogicalBytesIn.valueForLegacyUse());
    b.append("bytesOut", _ingressLogicalBytesOut.valueForLegacyUse());
    b.append("physicalBytesIn", static_cast<long long>(_ingressPhysicalBytesIn->loadRelaxed()));
    b.append("physicalBytesOut", static_cast<long long>(_ingressPhysicalBytesOut->loadRelaxed()));

    BSONObjBuilder egressBuilder(b.subobjStart("egress"));
    egressBuilder.append("bytesIn", _egressLogicalBytesIn.valueForLegacyUse());
    egressBuilder.append("bytesOut", _egressLogicalBytesOut.valueForLegacyUse());
    egressBuilder.append("physicalBytesIn",
                         static_cast<long long>(_egressPhysicalBytesIn->loadRelaxed()));
    egressBuilder.append("physicalBytesOut",
                         static_cast<long long>(_egressPhysicalBytesOut->loadRelaxed()));
    egressBuilder.append("numRequests", _egressNumRequests.valueForLegacyUse());
    egressBuilder.done();

    b.append("numSlowDNSOperations", _numSlowDNSOperations.valueForLegacyUse());
    b.append("numSlowSSLOperations", _numSlowSSLOperations.valueForLegacyUse());
    b.append("numRequests", _ingressNumRequests.valueForLegacyUse());

    BSONObjBuilder tfo;
#ifdef __linux__
    tfo.append("kernelSetting", _tfoKernelSetting);
#endif
    tfo.append("serverSupported", _tfoKernelSupportServer);
    tfo.append("clientSupported", _tfoKernelSupportClient);
    tfo.append("accepted", _tfoAccepted->loadRelaxed());
    b.append("tcpFastOpen", tfo.obj());
}

NetworkCounter& globalNetworkCounter() {
    static StaticImmortal<NetworkCounter> instance;
    return *instance;
}

void AuthCounter::initializeMechanismMap(const std::vector<std::string>& ingressMechanisms) {
    invariant(_mechanisms.empty());

    for (const auto& mech : {
             auth::kMechanismMongoX509,
             auth::kMechanismSaslPlain,
             auth::kMechanismGSSAPI,
             auth::kMechanismScramSha1,
             auth::kMechanismScramSha256,
             auth::kMechanismMongoAWS,
             auth::kMechanismMongoOIDC,
         }) {
        _mechanisms.emplace(std::piecewise_construct, std::tuple{mech}, std::tuple{});
    }

    for (const auto& mech : {
             // When clusterAuthMode == `x509` or `sendX509`, we'll use MONGODB-X509 for
             // intra-cluster auth even if it's not explicitly enabled by authenticationMechanisms.
             // Ensure it's always counted for ingress.
             auth::kMechanismMongoX509,

             // It's possible for intracluster auth to use a default fallback mechanism of
             // SCRAM-SHA-256 even if it's not configured to do so. Explicitly add this to the map
             // for now so that they can be incremented if this happens.
             auth::kMechanismScramSha256,
         }) {
        auto it = _mechanisms.find(mech);
        invariant(it != _mechanisms.end());
        it->second.ingressAllowed = true;
    }

    for (const auto& mech : ingressMechanisms) {
        auto it = _mechanisms.find(mech);
        uassert(12125800,
                fmt::format("Unknown mechanism {} present in authenticationMechanisms", mech),
                it != _mechanisms.end());
        it->second.ingressAllowed = true;
    }
}

void AuthCounter::incSaslSupportedMechanismsReceived() {
    _saslSupportedMechanismsReceived.fetchAndAddRelaxed(1);
}

void AuthCounter::incIngressAuthenticationCumulativeTime(long long micros) {
    _ingressAuthenticationCumulativeMicros.fetchAndAddRelaxed(micros);
}

void AuthCounter::incEgressAuthenticationCumulativeTime(long long micros) {
    _egressAuthenticationCumulativeMicros.fetchAndAddRelaxed(micros);
}


void AuthCounter::IngressMechanismCounterHandle::incSpeculativeAuthenticateReceived() {
    _data->ingress.speculativeAuthenticate.total.fetchAndAddRelaxed(1);
}

void AuthCounter::IngressMechanismCounterHandle::incIngressSpeculativeAuthenticateSuccessful() {
    _data->ingress.speculativeAuthenticate.successful.fetchAndAddRelaxed(1);
}

void AuthCounter::IngressMechanismCounterHandle::incAuthenticateReceived() {
    _data->ingress.authenticate.total.fetchAndAddRelaxed(1);
}

void AuthCounter::IngressMechanismCounterHandle::incIngressAuthenticateSuccessful() {
    _data->ingress.authenticate.successful.fetchAndAddRelaxed(1);
}

void AuthCounter::IngressMechanismCounterHandle::incClusterAuthenticateReceived() {
    _data->ingress.clusterAuthenticate.total.fetchAndAddRelaxed(1);
}

void AuthCounter::IngressMechanismCounterHandle::incClusterAuthenticateSuccessful() {
    _data->ingress.clusterAuthenticate.successful.fetchAndAddRelaxed(1);
}

void AuthCounter::EgressMechanismCounterHandle::incSpeculativeAuthenticateSent() {
    _data->egress.speculativeAuthenticate.total.fetchAndAddRelaxed(1);
}

void AuthCounter::EgressMechanismCounterHandle::incEgressSpeculativeAuthenticateSuccessful() {
    _data->egress.speculativeAuthenticate.successful.fetchAndAddRelaxed(1);
}

void AuthCounter::EgressMechanismCounterHandle::incAuthenticateSent() {
    _data->egress.authenticate.total.fetchAndAddRelaxed(1);
}

void AuthCounter::EgressMechanismCounterHandle::incEgressAuthenticateSuccessful() {
    _data->egress.authenticate.successful.fetchAndAddRelaxed(1);
}

auto AuthCounter::getEgressMechanismCounter(StringData mechanism) -> EgressMechanismCounterHandle {
    auto it = _mechanisms.find(mechanism);
    uassert(ErrorCodes::MechanismUnavailable,
            fmt::format("Egress authentication using mechanism {} which is not known", mechanism),
            it != _mechanisms.end());

    auto& data = it->second;
    return EgressMechanismCounterHandle(&data);
}

auto AuthCounter::getIngressMechanismCounter(StringData mechanism)
    -> IngressMechanismCounterHandle {
    auto it = _mechanisms.find(mechanism);
    uassert(ErrorCodes::MechanismUnavailable,
            fmt::format("Received authentication for mechanism {} which is not known", mechanism),
            it != _mechanisms.end());
    uassert(ErrorCodes::MechanismUnavailable,
            fmt::format("Received authentication for mechanism {} which is not enabled", mechanism),
            it->second.ingressAllowed);

    auto& data = it->second;
    return IngressMechanismCounterHandle(&data);
}

void AuthCounter::SuccessCounter::appendAsSubobj(BSONObjBuilder& bob, StringData fieldName) const {
    BSONObjBuilder subBob(bob.subobjStart(fieldName));
    subBob.append("total", total.load());
    subBob.append("successful", successful.load());
    subBob.done();
}

/**
 * authentication: {
 *   "mechanisms": {
 *     "SCRAM-SHA-256": {
 *       "ingress": {
 *         "speculativeAuthenticate": { total: ###, successful: ### },
 *         "clusterAuthenticate": { total: ###, successful: ### },
 *         "authenticate": { total: ###, successful: ### },
 *       },
 *       "egress": {
 *         "speculativeAuthenticate": { total: ###, successful: ### },
 *         "authenticate": { total: ###, successful: ### },
 *       },
 *     },
 *     ...
 *   },
 * }
 */
void AuthCounter::append(BSONObjBuilder* b) {
    const auto ssmReceived = _saslSupportedMechanismsReceived.load();
    b->append("saslSupportedMechsReceived", ssmReceived);

    BSONObjBuilder mechsBuilder(b->subobjStart("mechanisms"));

    for (const auto& it : _mechanisms) {
        BSONObjBuilder mechBuilder(mechsBuilder.subobjStart(it.first));
        if (it.second.ingressAllowed) {
            BSONObjBuilder ingressBuilder(mechBuilder.subobjStart("ingress"));
            it.second.ingress.speculativeAuthenticate.appendAsSubobj(
                ingressBuilder, auth::kSpeculativeAuthenticate);
            it.second.ingress.clusterAuthenticate.appendAsSubobj(ingressBuilder,
                                                                 auth::kClusterAuthenticate);
            it.second.ingress.authenticate.appendAsSubobj(ingressBuilder,
                                                          auth::kAuthenticateCommand);
            ingressBuilder.done();
        }

        BSONObjBuilder egressBuilder(mechBuilder.subobjStart("egress"));
        it.second.egress.speculativeAuthenticate.appendAsSubobj(egressBuilder,
                                                                auth::kSpeculativeAuthenticate);
        it.second.egress.authenticate.appendAsSubobj(egressBuilder, auth::kAuthenticateCommand);
        egressBuilder.done();

        mechBuilder.done();
    }

    mechsBuilder.done();

    const auto totalIngressAuthenticationTimeMicros = _ingressAuthenticationCumulativeMicros.load();
    b->append("totalIngressAuthenticationTimeMicros", totalIngressAuthenticationTimeMicros);
    const auto totalEgressAuthenticationTimeMicros = _egressAuthenticationCumulativeMicros.load();
    b->append("totalEgressAuthenticationTimeMicros", totalEgressAuthenticationTimeMicros);
}


AuthCounter authCounter;
AggStageCounters aggStageCounters{"aggStageCounters."};
DotsAndDollarsFieldsCounters dotsAndDollarsFieldsCounters;

namespace {
otel::metrics::Counter<int64_t>& makeOperationsCounter(otel::metrics::MetricName name,
                                                       StringData description,
                                                       StringData dottedPath,
                                                       bool skipPathValidation = false) {
    return MetricsService::instance().createInt64Counter(
        name,
        std::string(description),
        MetricUnit::kOperations,
        CounterOptions{.serverStatusOptions =
                           ServerStatusOptions{.dottedPath = std::string(dottedPath),
                                               .skipPathValidation = skipPathValidation}});
}
}  // namespace

PlanCacheCounters::PlanCacheCounters()
    : classicHits(
          makeOperationsCounter(MetricNames::kPlanCacheClassicHits,
                                "Number of times a plan was found in the classic plan cache.",
                                "query.planCache.classic.hits")),
      classicMisses(makeOperationsCounter(
          MetricNames::kPlanCacheClassicMisses,
          "Number of times no matching plan was found in the classic plan cache.",
          "query.planCache.classic.misses")),
      classicSkipped(makeOperationsCounter(
          MetricNames::kPlanCacheClassicSkipped,
          "Number of times the classic plan cache was not consulted for a query.",
          "query.planCache.classic.skipped")),
      classicReplanned(makeOperationsCounter(
          MetricNames::kPlanCacheClassicReplanned,
          "Number of times a cached classic plan was replanned after failing its trial run.",
          "query.planCache.classic.replanned")),
      classicReplannedPlanIsCachedPlan(
          makeOperationsCounter(MetricNames::kPlanCacheClassicReplannedPlanIsCachedPlan,
                                "Number of times replanning a cached classic plan produced the "
                                "same plan as the cached one.",
                                "query.planCache.classic.replanned_plan_is_cached_plan",
                                true)),
      classicCachedPlansEvicted(
          makeOperationsCounter(MetricNames::kPlanCacheClassicCachedPlansEvicted,
                                "Number of plans evicted from the classic plan cache.",
                                "query.planCache.classic.cached_plans_evicted",
                                true)),
      classicInactiveCachedPlansReplaced(
          makeOperationsCounter(MetricNames::kPlanCacheClassicInactiveCachedPlansReplaced,
                                "Number of times an inactive classic cached plan was replaced.",
                                "query.planCache.classic.inactive_cached_plans_replaced",
                                true)),
      sbeHits(makeOperationsCounter(MetricNames::kPlanCacheSbeHits,
                                    "Number of times a plan was found in the SBE plan cache.",
                                    "query.planCache.sbe.hits")),
      sbeMisses(
          makeOperationsCounter(MetricNames::kPlanCacheSbeMisses,
                                "Number of times no matching plan was found in the SBE plan cache.",
                                "query.planCache.sbe.misses")),
      sbeSkipped(
          makeOperationsCounter(MetricNames::kPlanCacheSbeSkipped,
                                "Number of times the SBE plan cache was not consulted for a query.",
                                "query.planCache.sbe.skipped")),
      sbeReplanned(makeOperationsCounter(
          MetricNames::kPlanCacheSbeReplanned,
          "Number of times a cached SBE plan was replanned after failing its trial run.",
          "query.planCache.sbe.replanned")),
      sbeReplannedPlanIsCachedPlan(makeOperationsCounter(
          MetricNames::kPlanCacheSbeReplannedPlanIsCachedPlan,
          "Number of times replanning the SBE engine produced the same plan as the cached one.",
          "query.planCache.sbe.replanned_plan_is_cached_plan",
          true)),
      sbeCachedPlansEvicted(
          makeOperationsCounter(MetricNames::kPlanCacheSbeCachedPlansEvicted,
                                "Number of plans evicted from the SBE plan cache.",
                                "query.planCache.sbe.cached_plans_evicted",
                                true)),
      sbeInactiveCachedPlansReplaced(
          makeOperationsCounter(MetricNames::kPlanCacheSbeInactiveCachedPlansReplaced,
                                "Number of times an inactive SBE cached plan was replaced.",
                                "query.planCache.sbe.inactive_cached_plans_replaced",
                                true)) {}

QueryFrameworkCounters::QueryFrameworkCounters()
    : sbeFindQueryCounter(makeOperationsCounter(MetricNames::kQueryFrameworkFindSbe,
                                                "Number of find queries executed fully using the "
                                                "SBE engine.",
                                                "query.queryFramework.find.sbe")),
      classicFindQueryCounter(makeOperationsCounter(MetricNames::kQueryFrameworkFindClassic,
                                                    "Number of find queries executed fully using "
                                                    "the classic engine.",
                                                    "query.queryFramework.find.classic")),
      sbeOnlyAggregationCounter(
          makeOperationsCounter(MetricNames::kQueryFrameworkAggregateSbeOnly,
                                "Number of aggregations fully pushed down to the SBE layer.",
                                "query.queryFramework.aggregate.sbeOnly")),
      classicOnlyAggregationCounter(
          makeOperationsCounter(MetricNames::kQueryFrameworkAggregateClassicOnly,
                                "Number of aggregations fully pushed down to the classic layer.",
                                "query.queryFramework.aggregate.classicOnly")),
      sbeHybridAggregationCounter(
          makeOperationsCounter(MetricNames::kQueryFrameworkAggregateSbeHybrid,
                                "Number of aggregations executed as SBE/DocumentSource hybrids.",
                                "query.queryFramework.aggregate.sbeHybrid")),
      classicHybridAggregationCounter(makeOperationsCounter(
          MetricNames::kQueryFrameworkAggregateClassicHybrid,
          "Number of aggregations executed as classic/DocumentSource hybrids.",
          "query.queryFramework.aggregate.classicHybrid")) {}

FastPathQueryCounters::FastPathQueryCounters()
    : idHackQueryCounter(makeOperationsCounter(MetricNames::kFastPathIdHack,
                                               "Number of queries planned using idHack fast "
                                               "planning.",
                                               "query.planning.fastPath.idHack")),
      expressQueryCounter(makeOperationsCounter(MetricNames::kFastPathExpress,
                                                "Number of queries planned using express fast "
                                                "planning.",
                                                "query.planning.fastPath.express")) {}

QueryFrameworkCounters queryFrameworkCounters;
LookupPushdownCounters lookupPushdownCounters;
LookupUnwindPushdownCounters lookupUnwindPushdownCounters;
ValidatorCounters validatorCounters;
ValidationLevelCounters validationLevelCounters;
GroupCounters groupCounters;
SetWindowFieldsCounters setWindowFieldsCounters;
GraphLookupCounters graphLookupCounters;
TextOrCounters textOrCounters;
BucketAutoCounters bucketAutoCounters;
GeoNearCounters geoNearCounters;
HashJoinCounters hashJoinCounters;
TimeseriesCounters timeseriesCounters;
OrCounters orCounters;
SortMergeCounters sortMergeCounters;
IxScanCounters ixScanCounters;
UniqueCounters uniqueCounters;
UniqueRoaringCounters uniqueRoaringCounters;
CountScanCounters countScanCounters;
NearCounters nearCounters;
UpdateCounters updateCounters;
PlanCacheCounters planCacheCounters;
FastPathQueryCounters fastPathQueryCounters;

OperatorCounters operatorCountersAggExpressions{"operatorCounters.expressions."};
OperatorCounters operatorCountersMatchExpressions{"operatorCounters.match."};
OperatorCounters operatorCountersGroupAccumulatorExpressions{"operatorCounters.groupAccumulators."};
OperatorCounters operatorCountersWindowAccumulatorExpressions{
    "operatorCounters.windowAccumulators."};

namespace {
template <ClusterRole::Value role>
QueryCounters queryCounterSingleton{role};
}  // namespace

QueryCounters& getQueryCounters(OperationContext* opCtx) {
    auto role = opCtx->getService()->role();
    if (role.hasExclusively(ClusterRole::ShardServer))
        return queryCounterSingleton<ClusterRole::ShardServer>;
    if (role.hasExclusively(ClusterRole::RouterServer))
        return queryCounterSingleton<ClusterRole::RouterServer>;
    MONGO_UNREACHABLE;
}

}  // namespace mongo
