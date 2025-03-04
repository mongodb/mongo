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

#include <absl/container/flat_hash_set.h>
#include <boost/optional.hpp>
#include <cstddef>
#include <fmt/format.h>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/admission/ingress_admission_context.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop_bson_helpers.h"
#include "mongo/db/prepare_conflict_tracker.h"
#include "mongo/db/profile_filter.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

namespace {

auto& oplogGetMoreStats = *MetricBuilder<TimerStats>("repl.network.oplogGetMoresProcessed");

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

}  // namespace

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

        tassert(7663403,
                str::stream() << "SerializationContext on the expCtx should not be empty, with ns: "
                              << expCtx->getNamespaceString().toStringForErrorMsg(),
                expCtx->getSerializationContext() != SerializationContext::stateDefault());

        // reportState is used to generate a command reply
        auto sc = SerializationContext::stateCommandReply(expCtx->getSerializationContext());
        CurOp::get(clientOpCtx)->reportState(infoBuilder, sc, truncateOps);

        if (const auto& queryShapeHash = CurOp::get(clientOpCtx)->getQueryShapeHash()) {
            infoBuilder->append("queryShapeHash", queryShapeHash->toHexString());
        }
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
    if (parent() != nullptr)
        parent()->yielded(_numYields.load());
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

void CurOp::_fetchStorageStatsIfNecessary(Date_t deadline, AdmissionContext::Priority priority) {
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
        ScopedAdmissionPriority<ExecutionAdmissionContext> admissionControl(opCtx, priority);
        Lock::GlobalLock lk(opCtx,
                            MODE_IS,
                            deadline,
                            Lock::InterruptBehavior::kThrow,
                            Lock::GlobalLockSkipOptions{.skipRSTLLock = true});
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

        try {
            // If we need them, try to fetch the storage stats. We use an unlimited timeout here,
            // but the lock acquisition could still be interrupted, which we catch and log.
            // We need to be careful of the priority, it has to match that of this operation.
            // If we choose a fixed priority other than kExempt (e.g., kNormal), it may
            // be lower than the operation's current priority, which would cause an exception to be
            // thrown.
            const auto& admCtx = ExecutionAdmissionContext::get(opCtx());
            _fetchStorageStatsIfNecessary(Date_t::max(), admCtx.getPriority());
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
    _message = message.toString();  // copy
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
        _progressMeter.emplace(progressMeterTotal, secondsBetween, 100, "", message.toString());
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

void CurOp::updateStorageMetricsOnRecoveryUnitUnstash(ClientLock&) {
    if (auto storageMetrics = shard_role_details::getRecoveryUnit(opCtx())->getStorageMetrics();
        !storageMetrics.isEmpty()) {
        _initializeResourceStatsBaseIfNecessary();
        _resourceStatsBase->storageMetrics += storageMetrics;
    }
}

void CurOp::updateStorageMetricsOnRecoveryUnitStash(ClientLock&) {
    if (auto storageMetrics = shard_role_details::getRecoveryUnit(opCtx())->getStorageMetrics();
        !storageMetrics.isEmpty()) {
        _initializeResourceStatsBaseIfNecessary();
        _resourceStatsBase->storageMetrics -= storageMetrics;
    }
}

void CurOp::setMemoryTrackingStats(const int64_t inUseMemoryBytes,
                                   const int64_t maxUsedMemoryBytes) {
    tassert(9897000,
            "featureFlagQueryMemoryTracking must be turned on before writing memory stats to CurOp",
            feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled());
    // We recompute the max here (the memory tracker that calls this method will already computing
    // the max) in order to avoid writing to the atomic if the max does not change.
    //
    // Set the max first, so that we can maintain the invariant that the max is always equal to or
    // greater than the current in-use tally.
    if (maxUsedMemoryBytes > _maxUsedMemoryBytes.load()) {
        _maxUsedMemoryBytes.store(maxUsedMemoryBytes);
    }

    _inUseMemoryBytes.store(inUseMemoryBytes);
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

    // The '_start' value is initialized to 0 and gets assigned on demand the first time it gets
    // accessed. The above thread ownership requirement ensures that there will never be
    // concurrent calls to this '_start' assignment, but we use compare-exchange anyway as an
    // additional check that writes to '_start' never race.
    TickSource::Tick unassignedStart = 0;
    invariant(_start.compare_exchange_strong(unassignedStart, _tickSource->getTicks()));
    return _start.load();
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
    auto prepareConflictDurationMicros =
        PrepareConflictTracker::get(opCtx()).getThisOpPrepareConflictDuration();
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
            _fetchStorageStatsIfNecessary(Date_t::now() + Milliseconds(500),
                                          AdmissionContext::Priority::kExempt);
        } catch (const DBException& ex) {
            LOGV2_OPTIONS(20526,
                          logOptions,
                          "Failed to gather storage statistics for slow operation",
                          "opId"_attr = opCtx->getOpID(),
                          "error"_attr = redact(ex));
        }

        // Gets the time spent blocked on prepare conflicts.
        auto prepareConflictDurationMicros =
            PrepareConflictTracker::get(opCtx).getThisOpPrepareConflictDuration();
        _debug.prepareConflictDurationMillis =
            duration_cast<Milliseconds>(prepareConflictDurationMicros);

        auto operationMetricsPtr = [&]() -> ResourceConsumption::OperationMetrics* {
            auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
            if (metricsCollector.hasCollectedMetrics()) {
                return &metricsCollector.getMetrics();
            }
            return nullptr;
        }();

        auto storageMetrics = getOperationStorageMetrics();

        logv2::DynamicAttributes attr;
        _debug.report(opCtx, &lockStats, operationMetricsPtr, storageMetrics, &attr);

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


    auto storageMetrics = getOperationStorageMetrics();
    if (auto n = storageMetrics.prepareReadConflicts; n > 0) {
        builder->append("prepareReadConflicts", n);
    }

    if (auto n = _debug.additiveMetrics.writeConflicts.load(); n > 0) {
        builder->append("writeConflicts", n);
    }
    if (auto n = _debug.additiveMetrics.temporarilyUnavailableErrors.load(); n > 0) {
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
    populateCurrentOpQueueStats(opCtx, _tickSource, builder);
}

CurOp::AdditiveResourceStats CurOp::getAdditiveResourceStats(
    const boost::optional<ExecutionAdmissionContext>& admCtx) {
    CurOp::AdditiveResourceStats stats;

    auto locker = shard_role_details::getLocker(opCtx());
    stats.lockStats = locker->stats();
    stats.cumulativeLockWaitTime = Microseconds(stats.lockStats.getCumulativeWaitTimeMicros());
    stats.timeQueuedForFlowControl =
        Microseconds(locker->getFlowControlStats().timeAcquiringMicros);

    stats.storageMetrics = shard_role_details::getRecoveryUnit(opCtx())->getStorageMetrics();

    if (admCtx != boost::none) {
        stats.timeQueuedForTickets = admCtx->totalTimeQueuedMicros();
    }

    return stats;
}

SingleThreadedStorageMetrics CurOp::getOperationStorageMetrics() const {
    SingleThreadedStorageMetrics singleThreadedMetrics =
        shard_role_details::getRecoveryUnit(opCtx())->getStorageMetrics();
    if (_resourceStatsBase) {
        singleThreadedMetrics -= _resourceStatsBase->storageMetrics;
    }
    return singleThreadedMetrics;
}

void CurOp::AdditiveResourceStats::addForUnstash(const CurOp::AdditiveResourceStats& other) {
    lockStats.append(other.lockStats);
    storageMetrics += other.storageMetrics;
    cumulativeLockWaitTime += other.cumulativeLockWaitTime;
    timeQueuedForFlowControl += other.timeQueuedForFlowControl;
    // timeQueuedForTickets is intentionally excluded as it is tracked separately
}

void CurOp::AdditiveResourceStats::subtractForStash(const CurOp::AdditiveResourceStats& other) {
    lockStats.subtract(other.lockStats);
    storageMetrics -= other.storageMetrics;
    cumulativeLockWaitTime -= other.cumulativeLockWaitTime;
    timeQueuedForFlowControl -= other.timeQueuedForFlowControl;
    // timeQueuedForTickets is intentionally excluded as it is tracked separately
}
}  // namespace mongo
