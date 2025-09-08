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

// CHECK_LOG_REDACTION

#include "mongo/db/curop.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/admission/ingress_admission_context.h"
#include "mongo/db/admission/ticketholder_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/curop_bson_helpers.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context_options_gen.h"
#include "mongo/db/profile_filter.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/prepare_conflict_tracker.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/random.h"
#include "mongo/rpc/metadata/audit_user_attrs.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/transport/service_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/ticketholder_queue_stats.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log_with_sampling.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <tuple>

#include <absl/container/flat_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {
auto& oplogGetMoreStats = *MetricBuilder<TimerStats>("repl.network.oplogGetMoresProcessed");

// Server operations are expected to regularly call OperationContext::checkForInterrupt() to see
// whether they have been killed and should halt execution. These counters track how many interrupt
// checks occur, over all operations, as well as information about overdue interrupt checks.

// Total interrupt checks across all operations, regardless of sampling.
auto& totalInterruptChecks = *MetricBuilder<Counter64>("operation.interrupt.totalChecks");

// Counts how many operations participated in interrupt check tracking.
auto& numSampledOps = *MetricBuilder<Counter64>("operation.interrupt.sampledOps");

// Accumulates total number of interrupt checks across all sampled operations.
auto& totalInterruptChecksFromSampledOps =
    *MetricBuilder<Counter64>("operation.interrupt.checksFromSample");

// Counts how many sampled operations had at least one overdue interrupt check.
auto& opsWithOverdueInterruptCheck =
    *MetricBuilder<Counter64>("operation.interrupt.overdueOpsFromSample");

// Counts total number of overdue interrupt checks across all sampled operations.
auto& overdueInterruptChecks =
    *MetricBuilder<Counter64>("operation.interrupt.overdueChecksFromSample");

// Computes total time overdue for interrupt checks across sampled operations.
auto& overdueInterruptTotalTimeMillis =
    *MetricBuilder<Counter64>("operation.interrupt.overdueInterruptTotalMillisFromSample");

// Approximate max time any sampled operation was overdue for interrupt check.
auto& overdueInterruptApproxMaxTimeMillis =
    *MetricBuilder<Atomic64Metric>("operation.interrupt.overdueInterruptApproxMaxMillisFromSample");

/*
 * Helper for reporting stats on an operation that was sampled for interrupt check tracking.
 */
void reportCheckForInterruptSampledOperation(
    int64_t numInterruptChecks, const OperationContext::OverdueInterruptCheckStats& stats) {
    totalInterruptChecksFromSampledOps.increment(numInterruptChecks);
    numSampledOps.increment();

    if (stats.overdueInterruptChecks.loadRelaxed() > 0) {
        opsWithOverdueInterruptCheck.increment();

        overdueInterruptChecks.increment(stats.overdueInterruptChecks.loadRelaxed());
        overdueInterruptTotalTimeMillis.increment(
            durationCount<Milliseconds>(stats.overdueAccumulator.loadRelaxed()));

        // Note that if we wanted the exact maximum, we would use a CAS loop, since it's possible a
        // new maximum will be entered between the set() and get() here. The approximate maximum is
        // good enough though.
        overdueInterruptApproxMaxTimeMillis.set(std::max(
            overdueInterruptApproxMaxTimeMillis.get(),
            static_cast<int64_t>(durationCount<Milliseconds>(stats.overdueMaxTime.loadRelaxed()))));
    }
}

/*
 * Helper for reporting stats on checkForInterrupt(), for any operation.
 */
void reportCheckForInterruptStats(OperationContext* opCtx) {
    const int64_t numInterruptChecks = opCtx->numInterruptChecks();
    totalInterruptChecks.increment(numInterruptChecks);

    if (auto* stats = opCtx->overdueInterruptCheckStats()) {
        // We treat this as a "last interrupt check," though we don't actually check for
        // interrupt since the operation is completing anyways.
        opCtx->updateInterruptCheckCounters();

        reportCheckForInterruptSampledOperation(numInterruptChecks, *stats);
    }
}

BSONObj serializeDollarDbInOpDescription(boost::optional<TenantId> tenantId,
                                         const BSONObj& cmdObj,
                                         const SerializationContext& sc) {
    auto db = cmdObj["$db"];
    if (!db) {
        return cmdObj;
    }

    auto dbName = DatabaseNameUtil::deserialize(tenantId, db.String(), sc);
    auto newCmdObj = cmdObj.addField(BSON("$db" << DatabaseNameUtil::serialize(
                                              dbName, SerializationContext::stateCommandReply(sc)))
                                         .firstElement());
    return newCmdObj;
}

void incrementQueueStats(TicketHolder* ticketHolder,
                         const ExecutionAdmissionContext::DelinquencyStats& stats) {
    ticketHolder->incrementDelinquencyStats(
        stats.delinquentAcquisitions.loadRelaxed(),
        Milliseconds(stats.totalAcquisitionDelinquencyMillis.loadRelaxed()),
        Milliseconds(stats.maxAcquisitionDelinquencyMillis.loadRelaxed()));
}
}  // namespace

Counter64& CurOp::totalInterruptChecks_forTest() {
    return totalInterruptChecks;
}
Counter64& CurOp::opsWithOverdueInterruptCheck_forTest() {
    return opsWithOverdueInterruptCheck;
}

/**
 * This type decorates a Client object with a stack of active CurOp objects.
 *
 * It encapsulates the nesting logic for curops attached to a Client, along with
 * the notion that there is always a root CurOp attached to a Client.
 *
 * The stack itself is represented in the _parent pointers of the CurOp class.
 */
class CurOp::CurOpStack {
    CurOpStack(const CurOpStack&) = delete;
    CurOpStack& operator=(const CurOpStack&) = delete;

public:
    CurOpStack() {
        _pushNoLock(&_base);
    }

    /**
     * Returns the top of the CurOp stack.
     */
    CurOp* top() const {
        return _top;
    }

    /**
     * Adds "curOp" to the top of the CurOp stack for a client.
     *
     * This sets the "_parent", "_stack", and "_lockStatsBase" fields
     * of "curOp".
     */
    void push(CurOp* curOp) {
        stdx::lock_guard<Client> lk(*opCtx()->getClient());
        _pushNoLock(curOp);
    }

    /**
     * Pops the top off the CurOp stack for a Client. Called by CurOp's destructor.
     */
    CurOp* pop() {
        // It is not necessary to lock when popping the final item off of the curop stack. This
        // is because the item at the base of the stack is owned by the stack itself, and is not
        // popped until the stack is being destroyed.  By the time the stack is being destroyed,
        // no other threads can be observing the Client that owns the stack, because it has been
        // removed from its ServiceContext's set of owned clients.  Further, because the last
        // item is popped in the destructor of the stack, and that destructor runs during
        // destruction of the owning client, it is not safe to access other member variables of
        // the client during the final pop.
        const bool shouldLock = _top->_parent;
        if (shouldLock) {
            opCtx()->getClient()->lock();
        }
        invariant(_top);
        CurOp* retval = _top;
        _top = _top->_parent;
        if (shouldLock) {
            opCtx()->getClient()->unlock();
        }
        return retval;
    }

    OperationContext* opCtx() {
        auto ctx = _curopStack.owner(this);
        invariant(ctx);
        return ctx;
    }

private:
    void _pushNoLock(CurOp* curOp) {
        invariant(!curOp->_parent);
        curOp->_stack = this;
        curOp->_parent = _top;

        // If `curOp` is a sub-operation, we store the snapshot of lock stats as the base lock stats
        // of the current operation. Also store the current ticket wait time as the base ticket
        // wait time.
        if (_top) {
            const boost::optional<ExecutionAdmissionContext> admCtx =
                ExecutionAdmissionContext::get(opCtx());
            curOp->_resourceStatsBase = curOp->getAdditiveResourceStats(admCtx);
        }

        _top = curOp;
    }

    // Top of the stack of CurOps for a Client.
    CurOp* _top = nullptr;

    // The bottom-most CurOp for a client.
    CurOp _base;
};

const OperationContext::Decoration<CurOp::CurOpStack> CurOp::_curopStack =
    OperationContext::declareDecoration<CurOp::CurOpStack>();

void CurOp::push(OperationContext* opCtx) {
    _curopStack(opCtx).push(this);
}

CurOp* CurOp::get(const OperationContext* opCtx) {
    return get(*opCtx);
}

CurOp* CurOp::get(const OperationContext& opCtx) {
    return _curopStack(opCtx).top();
}

void CurOp::reportCurrentOpForClient(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     Client* client,
                                     bool truncateOps,
                                     BSONObjBuilder* infoBuilder) {
    invariant(client);

    OperationContext* clientOpCtx = client->getOperationContext();

    infoBuilder->append("type", "op");

    const std::string hostName = prettyHostNameAndPort(client->getLocalPort());
    infoBuilder->append("host", hostName);

    client->reportState(*infoBuilder);
    if (auto clientMetadata = ClientMetadata::get(client)) {
        auto appName = clientMetadata->getApplicationName();
        if (!appName.empty()) {
            infoBuilder->append("appName", appName);
        }

        auto clientMetadataDocument = clientMetadata->getDocument();
        infoBuilder->append("clientMetadata", clientMetadataDocument);
    }

    // Fill out the rest of the BSONObj with opCtx specific details.
    infoBuilder->appendBool("active", client->hasAnyActiveCurrentOp());
    infoBuilder->append("currentOpTime",
                        expCtx->getOperationContext()
                            ->getServiceContext()
                            ->getPreciseClockSource()
                            ->now()
                            .toString());

    if (auto clientAuditUserAttrs = rpc::AuditUserAttrs::get(clientOpCtx)) {
        BSONArrayBuilder users(infoBuilder->subarrayStart("effectiveUsers"));
        clientAuditUserAttrs->getUser().serializeToBSON(&users);
        users.doneFast();
        if (clientAuditUserAttrs->getIsImpersonating()) {
            auto authSession = AuthorizationSession::get(client);
            if (authSession->isAuthenticated()) {
                BSONArrayBuilder users(infoBuilder->subarrayStart("runBy"));
                authSession->getAuthenticatedUserName()->serializeToBSON(&users);
            }
        }
    }

    infoBuilder->appendBool("isFromUserConnection", client->isFromUserConnection());

    if (transport::ServiceExecutorContext::get(client)) {
        infoBuilder->append("threaded"_sd, true);
    }

    if (clientOpCtx) {
        infoBuilder->append("opid", static_cast<int>(clientOpCtx->getOpID()));

        if (auto opKey = clientOpCtx->getOperationKey()) {
            opKey->appendToBuilder(infoBuilder, "operationKey");
        }

        if (clientOpCtx->isKillPending()) {
            infoBuilder->append("killPending", true);
        }

        if (auto lsid = clientOpCtx->getLogicalSessionId()) {
            BSONObjBuilder lsidBuilder(infoBuilder->subobjStart("lsid"));
            lsid->serialize(&lsidBuilder);
        }

        if (auto& vCtx = VersionContext::getDecoration(clientOpCtx); vCtx.isInitialized()) {
            infoBuilder->append("versionContext", vCtx.toBSON());
        }

        tassert(7663403,
                str::stream() << "SerializationContext on the expCtx should not be empty, with ns: "
                              << expCtx->getNamespaceString().toStringForErrorMsg(),
                expCtx->getSerializationContext() != SerializationContext::stateDefault());

        // reportState is used to generate a command reply
        auto sc = SerializationContext::stateCommandReply(expCtx->getSerializationContext());
        CurOp::get(clientOpCtx)->reportState(infoBuilder, sc, truncateOps);
    }

    if (expCtx->getOperationContext()->routedByReplicaSetEndpoint()) {
        // On the replica set endpoint, currentOp reports both router and shard operations so it
        // should label each op with its associated role.
        infoBuilder->append("role", toString(client->getService()->role()));
    }
}

bool CurOp::currentOpBelongsToTenant(Client* client, TenantId tenantId) {
    invariant(client);

    OperationContext* clientOpCtx = client->getOperationContext();

    if (!clientOpCtx || (CurOp::get(clientOpCtx))->getNSS().tenantId() != tenantId) {
        return false;
    }

    return true;
}

OperationContext* CurOp::opCtx() {
    invariant(_stack);
    return _stack->opCtx();
}

OperationContext* CurOp::opCtx() const {
    invariant(_stack);
    return _stack->opCtx();
}

void CurOp::setOpDescription(WithLock, const BSONObj& opDescription) {
    _opDescription = opDescription;
}

void CurOp::setGenericCursor(WithLock, GenericCursor gc) {
    _genericCursor = std::move(gc);
}

CurOp::~CurOp() {
    if (parent() != nullptr) {
        parent()->yielded(_numYields.load());
    }
    invariant(!_stack || this == _stack->pop());
}

void CurOp::setGenericOpRequestDetails(
    WithLock, NamespaceString nss, const Command* command, BSONObj cmdObj, NetworkOp op) {
    // Set the _isCommand flags based on network op only. For legacy writes on mongoS, we
    // resolve them to OpMsgRequests and then pass them into the Commands path, so having a
    // valid Command* here does not guarantee that the op was issued from the client using a
    // command protocol.
    const bool isCommand = (op == dbMsg || (op == dbQuery && nss.isCommand()));
    auto logicalOp = (command ? command->getLogicalOp() : networkOpToLogicalOp(op));

    _isCommand = _debug.iscommand = isCommand;
    _logicalOp = _debug.logicalOp = logicalOp;
    _networkOp = _debug.networkOp = op;
    _opDescription = cmdObj;
    _command = command;
    _nss = std::move(nss);
}

void CurOp::_fetchStorageStatsIfNecessary(Date_t deadline) {
    auto opCtx = this->opCtx();
    // Do not fetch operation statistics again if we have already got them (for
    // instance, as a part of stashing the transaction). Take a lock before calling into
    // the storage engine to prevent racing against a shutdown. Any operation that used
    // a storage engine would have at-least held a global lock at one point, hence we
    // limit our lock acquisition to such operations. We can get here and our lock
    // acquisition be timed out or interrupted, in which case we'll throw. Callers should
    // handle that case, e.g., by logging a message.
    if (_debug.storageStats == nullptr &&
        shard_role_details::getLocker(opCtx)->wasGlobalLockTaken() &&
        opCtx->getServiceContext()->getStorageEngine()) {
        ScopedAdmissionPriority<ExecutionAdmissionContext> admissionControl(
            opCtx, AdmissionContext::Priority::kExempt);
        Lock::GlobalLock lk(opCtx,
                            MODE_IS,
                            deadline,
                            Lock::InterruptBehavior::kThrow,
                            Lock::GlobalLockOptions{.skipRSTLLock = true});
        _debug.storageStats =
            shard_role_details::getRecoveryUnit(opCtx)->computeOperationStatisticsSinceLastCall();
    }
}

void CurOp::setEndOfOpMetrics(long long nreturned) {
    _debug.additiveMetrics.nreturned = nreturned;
    // A non-none queryStatsInfo.keyHash indicates the current query is being tracked locally for
    // queryStats, and a metricsRequested being true indicates the query is being tracked remotely
    // via the metrics included in cursor responses. In either case, we need to track the current
    // working and storage metrics, as they are recorded in the query stats store and returned
    // in cursor responses. When tracking locally, we also need to record executionTime.
    // executionTime is set with the final executionTime in completeAndLogOperation, but
    // for query stats collection we want it set before incrementing cursor metrics using OpDebug's
    // AdditiveMetrics. The value of executionTime set here will be overwritten later in
    // completeAndLogOperation.
    const auto& info = _debug.queryStatsInfo;
    if (info.keyHash || info.metricsRequested) {
        auto& metrics = _debug.additiveMetrics;
        auto elapsed = elapsedTimeExcludingPauses();
        // We don't strictly need to record executionTime unless keyHash is non-none, but there's
        // no harm in recording it since we've already computed the value.
        metrics.executionTime = elapsed;
        metrics.clusterWorkingTime = metrics.clusterWorkingTime.value_or(Milliseconds(0)) +
            (duration_cast<Milliseconds>(elapsed - (_sumBlockedTimeTotal() - _blockedTimeAtStart)));

        calculateCpuTime();
        metrics.cpuNanos = metrics.cpuNanos.value_or(Nanoseconds(0)) + _debug.cpuTime;

        if (const auto& admCtx = ExecutionAdmissionContext::get(opCtx());
            admCtx.getDelinquentAcquisitions() > 0 && !opCtx()->inMultiDocumentTransaction() &&
            !parent()) {
            // Note that we don't record delinquency stats around ticketing when in a
            // multi-document transaction, since operations within multi-document transactions hold
            // tickets for a long time by design and reporting them as delinquent will just create
            // noise in the data.

            metrics.delinquentAcquisitions = metrics.delinquentAcquisitions.value_or(0) +
                static_cast<uint64_t>(admCtx.getDelinquentAcquisitions());
            metrics.totalAcquisitionDelinquency =
                metrics.totalAcquisitionDelinquency.value_or(Milliseconds(0)) +
                Milliseconds(admCtx.getTotalAcquisitionDelinquencyMillis());
            metrics.maxAcquisitionDelinquency = Milliseconds{
                std::max(metrics.maxAcquisitionDelinquency.value_or(Milliseconds(0)).count(),
                         admCtx.getMaxAcquisitionDelinquencyMillis())};
        }

        if (!parent()) {
            metrics.numInterruptChecks = opCtx()->numInterruptChecks();
            if (const auto* stats = opCtx()->overdueInterruptCheckStats();
                stats && stats->overdueInterruptChecks.loadRelaxed() > 0) {
                metrics.overdueInterruptApproxMax =
                    std::max(metrics.overdueInterruptApproxMax.value_or(Milliseconds(0)),
                             stats->overdueMaxTime.loadRelaxed());
            }
        }

        try {
            // If we need them, try to fetch the storage stats. We use an unlimited timeout here,
            // but the lock acquisition could still be interrupted, which we catch and log.
            // We need to be careful of the priority, it has to match that of this operation.
            // If we choose a fixed priority other than kExempt (e.g., kNormal), it may
            // be lower than the operation's current priority, which would cause an exception to be
            // thrown.
            _fetchStorageStatsIfNecessary(Date_t::max());
        } catch (DBException& ex) {
            LOGV2(8457400,
                  "Failed to gather storage statistics for query stats",
                  "opId"_attr = opCtx()->getOpID(),
                  "error"_attr = redact(ex));
        }

        if (_debug.storageStats) {
            metrics.aggregateStorageStats(*_debug.storageStats);
        }
    }
}

void CurOp::setMessage(WithLock, StringData message) {
    if (_progressMeter && _progressMeter->isActive()) {
        LOGV2_ERROR(
            20527, "Updating message", "old"_attr = redact(_message), "new"_attr = redact(message));
        MONGO_verify(!_progressMeter->isActive());
    }
    _message = std::string{message};  // copy
}

ProgressMeter& CurOp::setProgress(WithLock lk,
                                  StringData message,
                                  unsigned long long progressMeterTotal,
                                  int secondsBetween) {
    setMessage(lk, message);
    if (_progressMeter) {
        _progressMeter->reset(progressMeterTotal, secondsBetween);
        _progressMeter->setName(message);
    } else {
        _progressMeter.emplace(progressMeterTotal, secondsBetween, 100, "", std::string{message});
    }

    return _progressMeter.value();
}

void CurOp::updateStatsOnTransactionUnstash(ClientLock&) {
    // Store lock stats and storage metrics from the locker and recovery unit after unstashing.
    // These stats have accrued outside of this CurOp instance so we will ignore/subtract them when
    // reporting on this operation.
    _initializeResourceStatsBaseIfNecessary();
    _resourceStatsBase->addForUnstash(getAdditiveResourceStats(boost::none));
}

void CurOp::updateStatsOnTransactionStash(ClientLock&) {
    // Store lock stats and storage metrics that happened during this operation before the locker
    // and recovery unit are stashed. We take the delta of the stats before stashing and the base
    // stats which includes the snapshot of stats when it was unstashed. This stats delta on
    // stashing is added when reporting on this operation.
    _initializeResourceStatsBaseIfNecessary();
    _resourceStatsBase->subtractForStash(getAdditiveResourceStats(boost::none));
}

void CurOp::setMemoryTrackingStats(const int64_t inUseTrackedMemoryBytes,
                                   const int64_t peakTrackedMemoryBytes) {
    tassert(9897000,
            "featureFlagQueryMemoryTracking must be turned on before writing memory stats to CurOp",
            feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled());
    // We recompute the peak here (the memory tracker that calls this method will already computing
    // the peak) in order to avoid writing to the atomic if the peak does not change.
    //
    // Set peak memory first, so that we can maintain the invariant that the peak is always
    // equal to or greater than the current in-use tally.
    if (peakTrackedMemoryBytes > _peakTrackedMemoryBytes.load()) {
        _peakTrackedMemoryBytes.store(peakTrackedMemoryBytes);
    }

    _inUseTrackedMemoryBytes.store(inUseTrackedMemoryBytes);
}

void CurOp::setNS(WithLock, NamespaceString nss) {
    _nss = std::move(nss);
}

void CurOp::setNS(WithLock, const DatabaseName& dbName) {
    _nss = NamespaceString(dbName);
}

TickSource::Tick CurOp::startTime() {
    auto start = _start.load();
    if (start != 0) {
        return start;
    }

    // Start the CPU timer if this system supports it.
    if (auto cpuTimers = OperationCPUTimers::get(opCtx())) {
        _cpuTimer = cpuTimers->makeTimer();
        _cpuTimer->start();
    }

    _blockedTimeAtStart = _sumBlockedTimeTotal();

    TickSource::Tick startTime = _tickSource->getTicks();

    // For top level operations, we decide whether or not to sample this operation for interrupt
    // tracking.
    if (gFeatureFlagRecordDelinquentMetrics.isEnabled() && !parent() &&
        opCtx()->getClient()->getPrng().nextCanonicalDouble() <
            gOverdueInterruptCheckSamplingRate.loadRelaxed()) {
        opCtx()->trackOverdueInterruptChecks(startTime);
    }

    // The '_start' value is initialized to 0 and gets assigned on demand the first time it gets
    // accessed. The above thread ownership requirement ensures that there will never be
    // concurrent calls to this '_start' assignment, but we use compare-exchange anyway as an
    // additional check that writes to '_start' never race.
    TickSource::Tick unassignedStart = 0;
    invariant(_start.compare_exchange_strong(unassignedStart, startTime));

    return startTime;
}

void CurOp::done() {
    _end = _tickSource->getTicks();
}

void CurOp::calculateCpuTime() {
    if (_cpuTimer && _debug.cpuTime < Nanoseconds::zero()) {
        _debug.cpuTime = _cpuTimer->getElapsed();
    }
}

Microseconds CurOp::computeElapsedTimeTotal(TickSource::Tick startTime,
                                            TickSource::Tick endTime) const {
    invariant(startTime != 0);

    if (!endTime) {
        // This operation is ongoing.
        return _tickSource->ticksTo<Microseconds>(_tickSource->getTicks() - startTime);
    }

    return _tickSource->ticksTo<Microseconds>(endTime - startTime);
}

Milliseconds CurOp::_sumBlockedTimeTotal() {
    auto locker = shard_role_details::getLocker(opCtx());
    auto prepareConflictDurationMicros = StorageExecutionContext::get(opCtx())
                                             ->getPrepareConflictTracker()
                                             .getThisOpPrepareConflictDuration();
    auto cumulativeLockWaitTime = Microseconds(locker->stats().getCumulativeWaitTimeMicros());
    auto timeQueuedForTickets = ExecutionAdmissionContext::get(opCtx()).totalTimeQueuedMicros();
    auto timeQueuedForFlowControl = Microseconds(locker->getFlowControlStats().timeAcquiringMicros);

    if (_resourceStatsBase) {
        cumulativeLockWaitTime -= _resourceStatsBase->cumulativeLockWaitTime;
        timeQueuedForTickets -= _resourceStatsBase->timeQueuedForTickets;
        timeQueuedForFlowControl -= _resourceStatsBase->timeQueuedForFlowControl;
    }

    return duration_cast<Milliseconds>(cumulativeLockWaitTime + timeQueuedForTickets +
                                       timeQueuedForFlowControl + prepareConflictDurationMicros);
}

void CurOp::enter(WithLock, NamespaceString nss, int dbProfileLevel) {
    ensureStarted();
    _nss = std::move(nss);
    raiseDbProfileLevel(dbProfileLevel);
}

void CurOp::enter(WithLock lk, const DatabaseName& dbName, int dbProfileLevel) {
    enter(lk, NamespaceString(dbName), dbProfileLevel);
}

bool CurOp::shouldDBProfile() {
    // Profile level 2 should override any sample rate or slowms settings.
    if (_dbprofile >= 2)
        return true;

    if (_dbprofile <= 0)
        return false;

    auto& dbProfileSettings = DatabaseProfileSettings::get(opCtx()->getServiceContext());
    if (dbProfileSettings.getDatabaseProfileSettings(getNSS().dbName()).filter)
        return true;

    return elapsedTimeExcludingPauses() >= Milliseconds{serverGlobalParams.slowMS.load()};
}

void CurOp::raiseDbProfileLevel(int dbProfileLevel) {
    _dbprofile = std::max(dbProfileLevel, _dbprofile);
}

bool CurOp::shouldCurOpStackOmitDiagnosticInformation(CurOp* curop) {
    do {
        if (curop->getShouldOmitDiagnosticInformation()) {
            return true;
        }

        curop = curop->parent();
    } while (curop != nullptr);

    return false;
}

bool CurOp::completeAndLogOperation(const logv2::LogOptions& logOptions,
                                    std::shared_ptr<const ProfileFilter> filter,
                                    boost::optional<size_t> responseLength,
                                    boost::optional<long long> slowMsOverride,
                                    bool forceLog) {
    auto opCtx = this->opCtx();
    const long long slowMs = slowMsOverride.value_or(serverGlobalParams.slowMS.load());

    // Record the size of the response returned to the client, if applicable.
    if (responseLength) {
        _debug.responseLength = *responseLength;
    }

    // Obtain the total execution time of this operation.
    done();
    _debug.additiveMetrics.executionTime = elapsedTimeExcludingPauses();
    const auto executionTimeMillis =
        durationCount<Milliseconds>(*_debug.additiveMetrics.executionTime);

    if (!opCtx->inMultiDocumentTransaction()) {
        // If we're not in a txn, we record information about delinquent ticket acquisitions to the
        // Queue's stats.
        auto& admCtx = ExecutionAdmissionContext::get(opCtx);

        if (auto manager = admission::TicketHolderManager::get(opCtx->getServiceContext())) {
            incrementQueueStats(manager->getTicketHolder(LockMode::MODE_IS),
                                admCtx.readDelinquencyStats());
            incrementQueueStats(manager->getTicketHolder(LockMode::MODE_IX),
                                admCtx.writeDelinquencyStats());
        }
    }

    // For the top-level operation which was chosen for sampling, update the server status
    // counters. We don't want to double count overdue interrupt checks that come from child
    // operations in the serverStatus counters.
    if (!parent()) {
        reportCheckForInterruptStats(opCtx);
    }

    // Do not log the slow query information if asked to omit it
    if (shouldCurOpStackOmitDiagnosticInformation(this)) {
        return false;
    }

    if (_debug.isReplOplogGetMore) {
        oplogGetMoreStats.recordMillis(executionTimeMillis);
    }

    auto workingMillis =
        Milliseconds(executionTimeMillis) - (_sumBlockedTimeTotal() - _blockedTimeAtStart);
    // Round up to zero if necessary to allow precision errors from FastClockSource used by flow
    // control ticketholder.
    _debug.workingTimeMillis = (workingMillis < Milliseconds(0) ? Milliseconds(0) : workingMillis);

    bool shouldLogSlowOp, shouldProfileAtLevel1;

    if (filter) {
        // Calculate this operation's CPU time before deciding whether logging/profiling is
        // necessary only if it is needed for filtering.
        if (filter->dependsOn("cpuNanos")) {
            calculateCpuTime();
        }

        bool passesFilter = filter->matches(opCtx, _debug, *this);

        shouldLogSlowOp = passesFilter;
        shouldProfileAtLevel1 = passesFilter;

    } else {
        // Log the operation if it is eligible according to the current slowMS and sampleRate
        // settings.
        bool shouldSample;
        std::tie(shouldLogSlowOp, shouldSample) = shouldLogSlowOpWithSampling(
            opCtx, logOptions.component(), _debug.workingTimeMillis, Milliseconds(slowMs));

        shouldProfileAtLevel1 = shouldLogSlowOp && shouldSample;
    }

    // Defer calculating the CPU time until we know that we actually are going to write it to
    // the logs or profiler. The CPU time may have been determined earlier if it was a
    // dependency of 'filter' in which case this is a no-op.
    if (forceLog || shouldLogSlowOp || _dbprofile >= 2) {
        calculateCpuTime();
    }

    if (forceLog || shouldLogSlowOp) {
        auto locker = shard_role_details::getLocker(opCtx);
        SingleThreadedLockStats lockStats(locker->stats());

        try {
            // Slow query logs are critical for observability and should not wait for ticket
            // acquisition. Slow queries can happen for various reasons; however, if queries
            // are slower due to ticket exhaustion, queueing in order to log can compound
            // the issue. Hence we pass the kExempt priority to _fetchStorageStatsIfNecessary.
            _fetchStorageStatsIfNecessary(Date_t::now() + Milliseconds(500));
        } catch (const DBException& ex) {
            LOGV2_OPTIONS(20526,
                          logOptions,
                          "Failed to gather storage statistics for slow operation",
                          "opId"_attr = opCtx->getOpID(),
                          "error"_attr = redact(ex));
        }

        // Gets the time spent blocked on prepare conflicts.
        auto prepareConflictDurationMicros = StorageExecutionContext::get(opCtx)
                                                 ->getPrepareConflictTracker()
                                                 .getThisOpPrepareConflictDuration();
        _debug.prepareConflictDurationMillis =
            duration_cast<Milliseconds>(prepareConflictDurationMicros);

        const auto& storageMetrics = getOperationStorageMetrics();

        logv2::DynamicAttributes attr;
        _debug.report(opCtx, &lockStats, storageMetrics, getPrepareReadConflicts(), &attr);

        LOGV2_OPTIONS(51803, logOptions, "Slow query", attr);

        _checkForFailpointsAfterCommandLogged();
    }

    // Return 'true' if this operation should also be added to the profiler.
    if (_dbprofile >= 2)
        return true;
    if (_dbprofile <= 0)
        return false;
    return shouldProfileAtLevel1;
}

std::string CurOp::getNS() const {
    return NamespaceStringUtil::serialize(_nss, SerializationContext::stateDefault());
}

// Failpoints after commands are logged.
constexpr auto kPrepareTransactionCmdName = "prepareTransaction"_sd;
MONGO_FAIL_POINT_DEFINE(waitForPrepareTransactionCommandLogged);
constexpr auto kHelloCmdName = "hello"_sd;
MONGO_FAIL_POINT_DEFINE(waitForHelloCommandLogged);
constexpr auto kIsMasterCmdName = "isMaster"_sd;
MONGO_FAIL_POINT_DEFINE(waitForIsMasterCommandLogged);

void CurOp::_checkForFailpointsAfterCommandLogged() {
    if (!isCommand() || !getCommand()) {
        return;
    }

    auto cmdName = getCommand()->getName();
    if (cmdName == kPrepareTransactionCmdName) {
        if (MONGO_unlikely(waitForPrepareTransactionCommandLogged.shouldFail())) {
            LOGV2(31481, "waitForPrepareTransactionCommandLogged failpoint enabled");
        }
    } else if (cmdName == kHelloCmdName) {
        if (MONGO_unlikely(waitForHelloCommandLogged.shouldFail())) {
            LOGV2(31482, "waitForHelloCommandLogged failpoint enabled");
        }
    } else if (cmdName == kIsMasterCmdName) {
        if (MONGO_unlikely(waitForIsMasterCommandLogged.shouldFail())) {
            LOGV2(31483, "waitForIsMasterCommandLogged failpoint enabled");
        }
    }
}

Command::ReadWriteType CurOp::getReadWriteType() const {
    if (_command) {
        return _command->getReadWriteType();
    }
    switch (_logicalOp) {
        case LogicalOp::opGetMore:
        case LogicalOp::opQuery:
            return Command::ReadWriteType::kRead;
        case LogicalOp::opBulkWrite:
        case LogicalOp::opUpdate:
        case LogicalOp::opInsert:
        case LogicalOp::opDelete:
            return Command::ReadWriteType::kWrite;
        default:
            return Command::ReadWriteType::kCommand;
    }
}

namespace {

/**
 * Populates the BSONObjBuilder with the queueing statistics of the current operation. Calculates
 * overall queue stats and records the current queue if the operation is presently queued.
 */
void populateCurrentOpQueueStats(OperationContext* opCtx,
                                 TickSource* tickSource,
                                 BSONObjBuilder* currOpStats) {
    boost::optional<std::tuple<TicketHolderQueueStats::QueueType, Microseconds>> currentQueue;
    BSONObjBuilder queuesBuilder(currOpStats->subobjStart("queues"));

    for (auto&& [queueType, lookup] : TicketHolderQueueStats::getQueueMetricsRegistry()) {
        AdmissionContext* admCtx = lookup(opCtx);
        Microseconds totalTimeQueuedMicros = admCtx->totalTimeQueuedMicros();

        if (auto startQueueingTime = admCtx->startQueueingTime()) {
            Microseconds currentQueueTimeQueuedMicros = tickSource->ticksTo<Microseconds>(
                opCtx->getServiceContext()->getTickSource()->getTicks() - *startQueueingTime);
            totalTimeQueuedMicros += currentQueueTimeQueuedMicros;
            currentQueue = std::make_tuple(queueType, currentQueueTimeQueuedMicros);
        }
        BSONObjBuilder queueMetricsBuilder(
            queuesBuilder.subobjStart(TicketHolderQueueStats::queueTypeToString(queueType)));
        queueMetricsBuilder.append("admissions", admCtx->getAdmissions());
        queueMetricsBuilder.append("totalTimeQueuedMicros",
                                   durationCount<Microseconds>(totalTimeQueuedMicros));
        queueMetricsBuilder.append("isHoldingTicket", admCtx->isHoldingTicket());
        queueMetricsBuilder.done();
    }
    queuesBuilder.done();
    if (currentQueue) {
        BSONObjBuilder currentQueueBuilder(currOpStats->subobjStart("currentQueue"));
        currentQueueBuilder.append(
            "name", TicketHolderQueueStats::queueTypeToString(std::get<0>(*currentQueue)));
        currentQueueBuilder.append("timeQueuedMicros",
                                   durationCount<Microseconds>(std::get<1>(*currentQueue)));
        currentQueueBuilder.done();
    } else {
        currOpStats->appendNull("currentQueue");
    }
};
}  // namespace

BSONObj CurOp::truncateAndSerializeGenericCursor(GenericCursor cursor,
                                                 boost::optional<size_t> maxQuerySize) {
    if (maxQuerySize && cursor.getOriginatingCommand() &&
        static_cast<size_t>(cursor.getOriginatingCommand()->objsize()) > *maxQuerySize) {
        BSONObjBuilder truncatedBuilder;
        curop_bson_helpers::buildTruncatedObject(
            *cursor.getOriginatingCommand(), *maxQuerySize, truncatedBuilder);
        cursor.setOriginatingCommand(truncatedBuilder.obj());
    }

    // Remove fields that are present in the parent "curop" object.
    cursor.setLsid(boost::none);
    cursor.setNs(boost::none);
    cursor.setPlanSummary(boost::none);
    cursor.setInUseTrackedMemBytes(boost::none);
    cursor.setPeakTrackedMemBytes(boost::none);
    return cursor.toBSON();
}

void CurOp::reportState(BSONObjBuilder* builder,
                        const SerializationContext& serializationContext,
                        bool truncateOps) {
    auto opCtx = this->opCtx();
    auto start = _start.load();
    if (start) {
        auto end = _end.load();
        auto elapsedTimeTotal = computeElapsedTimeTotal(start, end);
        builder->append("secs_running", durationCount<Seconds>(elapsedTimeTotal));
        builder->append("microsecs_running", durationCount<Microseconds>(elapsedTimeTotal));
    }

    builder->append("op", logicalOpToString(_logicalOp));
    builder->append("ns", NamespaceStringUtil::serialize(_nss, serializationContext));

    bool omitAndRedactInformation = getShouldOmitDiagnosticInformation();
    builder->append("redacted", omitAndRedactInformation);

    // When the currentOp command is run, it returns a single response object containing all
    // current operations; this request will fail if the response exceeds the 16MB document
    // limit. By contrast, the $currentOp aggregation stage does not have this restriction. If
    // 'truncateOps' is true, limit the size of each op to 1000 bytes. Otherwise, do not
    // truncate.
    const boost::optional<size_t> maxQuerySize{truncateOps, 1000};

    auto obj = [&]() {
        if (!gMultitenancySupport) {
            return curop_bson_helpers::appendCommentField(opCtx, _opDescription);
        } else {
            return curop_bson_helpers::appendCommentField(
                opCtx,
                serializeDollarDbInOpDescription(
                    _nss.tenantId(), _opDescription, serializationContext));
        }
    }();

    // If flag is true, add command field to builder without sensitive information.
    if (omitAndRedactInformation) {
        BSONObjBuilder redactedCommandBuilder;
        redactedCommandBuilder.append(obj.firstElement());
        redactedCommandBuilder.append(obj["$db"]);
        auto commentElement = obj["comment"];
        if (commentElement.ok()) {
            redactedCommandBuilder.append(commentElement);
        }

        if (obj.firstElementFieldNameStringData() == "getMore"_sd) {
            redactedCommandBuilder.append(obj["collection"]);
        }

        curop_bson_helpers::appendObjectTruncatingAsNecessary(
            "command", redactedCommandBuilder.done(), maxQuerySize, *builder);
    } else {
        curop_bson_helpers::appendObjectTruncatingAsNecessary(
            "command", obj, maxQuerySize, *builder);
    }


    // Omit information for QE user collections, QE state collections and QE user operations.
    if (omitAndRedactInformation) {
        return;
    }

    switch (_debug.queryFramework) {
        case PlanExecutor::QueryFramework::kClassicOnly:
        case PlanExecutor::QueryFramework::kClassicHybrid:
            builder->append("queryFramework", "classic");
            break;
        case PlanExecutor::QueryFramework::kSBEOnly:
        case PlanExecutor::QueryFramework::kSBEHybrid:
            builder->append("queryFramework", "sbe");
            break;
        case PlanExecutor::QueryFramework::kUnknown:
            break;
    }

    if (!_planSummary.empty()) {
        builder->append("planSummary", _planSummary);
    }

    if (int64_t inUseTrackedMemoryBytes = getInUseTrackedMemoryBytes()) {
        builder->append("inUseTrackedMemBytes", inUseTrackedMemoryBytes);
    }

    if (int64_t peakTrackedMemoryBytes = getPeakTrackedMemoryBytes()) {
        builder->append("peakTrackedMemBytes", peakTrackedMemoryBytes);
    }

    if (_genericCursor) {
        builder->append("cursor", truncateAndSerializeGenericCursor(*_genericCursor, maxQuerySize));
    }

    if (!_message.empty()) {
        if (_progressMeter && _progressMeter->isActive()) {
            StringBuilder buf;
            buf << _message << " " << _progressMeter->toString();
            builder->append("msg", buf.str());
            BSONObjBuilder sub(builder->subobjStart("progress"));
            sub.appendNumber("done", (long long)_progressMeter->done());
            sub.appendNumber("total", (long long)_progressMeter->total());
            sub.done();
        } else {
            builder->append("msg", _message);
        }
    }

    if (!_failPointMessage.empty()) {
        builder->append("failpointMsg", _failPointMessage);
    }


    if (auto n = getPrepareReadConflicts(); n > 0) {
        builder->append("prepareReadConflicts", n);
    }

    auto storageMetrics = getOperationStorageMetrics();
    if (auto n = storageMetrics.writeConflicts; n > 0) {
        builder->append("writeConflicts", n);
    }
    if (auto n = storageMetrics.temporarilyUnavailableErrors; n > 0) {
        builder->append("temporarilyUnavailableErrors", n);
    }

    builder->append("numYields", _numYields.load());

    if (_debug.dataThroughputLastSecond) {
        builder->append("dataThroughputLastSecond", *_debug.dataThroughputLastSecond);
    }

    if (_debug.dataThroughputAverage) {
        builder->append("dataThroughputAverage", *_debug.dataThroughputAverage);
    }

    if (auto start = _waitForWriteConcernStart.load(); start > 0) {
        auto end = _waitForWriteConcernEnd.load();
        auto elapsedTimeTotal = _atomicWaitForWriteConcernDurationMillis.load();
        elapsedTimeTotal += duration_cast<Milliseconds>(computeElapsedTimeTotal(start, end));
        builder->append("waitForWriteConcernDurationMillis",
                        durationCount<Milliseconds>(elapsedTimeTotal));
    }

    if (!parent() && isStarted()) {
        builder->append("numInterruptChecks", opCtx->numInterruptChecks());
        const auto& admCtx = ExecutionAdmissionContext::get(opCtx);
        const auto* stats = opCtx->overdueInterruptCheckStats();
        if (admCtx.getDelinquentAcquisitions() > 0 ||
            (stats && stats->overdueInterruptChecks.loadRelaxed() > 0)) {
            BSONObjBuilder sub(builder->subobjStart("delinquencyInfo"));
            OpDebug::appendDelinquentInfo(opCtx, sub);
        }
    }

    populateCurrentOpQueueStats(opCtx, _tickSource, builder);

    if (auto&& queryShapeHash = _debug.getQueryShapeHash()) {
        builder->append("queryShapeHash", queryShapeHash->toHexString());
    }
}

CurOp::AdditiveResourceStats CurOp::getAdditiveResourceStats(
    const boost::optional<ExecutionAdmissionContext>& admCtx) {
    CurOp::AdditiveResourceStats stats;

    auto locker = shard_role_details::getLocker(opCtx());
    stats.lockStats = locker->stats();
    stats.cumulativeLockWaitTime = Microseconds(stats.lockStats.getCumulativeWaitTimeMicros());
    stats.timeQueuedForFlowControl =
        Microseconds(locker->getFlowControlStats().timeAcquiringMicros);

    if (admCtx != boost::none) {
        stats.timeQueuedForTickets = admCtx->totalTimeQueuedMicros();
    }

    return stats;
}

SingleThreadedStorageMetrics CurOp::getOperationStorageMetrics() const {
    return StorageExecutionContext::get(opCtx())->getStorageMetrics();
}

long long CurOp::getPrepareReadConflicts() const {
    return StorageExecutionContext::get(opCtx())
        ->getPrepareConflictTracker()
        .getThisOpPrepareConflictCount();
}

void CurOp::updateSpillStorageStats(std::unique_ptr<StorageStats> operationStorageStats) {
    if (!operationStorageStats) {
        return;
    }
    if (!_debug.spillStorageStats) {
        _debug.spillStorageStats = std::move(operationStorageStats);
        return;
    }
    *_debug.spillStorageStats += *operationStorageStats;
}

void CurOp::AdditiveResourceStats::addForUnstash(const CurOp::AdditiveResourceStats& other) {
    lockStats.append(other.lockStats);
    cumulativeLockWaitTime += other.cumulativeLockWaitTime;
    timeQueuedForFlowControl += other.timeQueuedForFlowControl;
    // timeQueuedForTickets is intentionally excluded as it is tracked separately
}

void CurOp::AdditiveResourceStats::subtractForStash(const CurOp::AdditiveResourceStats& other) {
    lockStats.subtract(other.lockStats);
    cumulativeLockWaitTime -= other.cumulativeLockWaitTime;
    timeQueuedForFlowControl -= other.timeQueuedForFlowControl;
    // timeQueuedForTickets is intentionally excluded as it is tracked separately
}
}  // namespace mongo
