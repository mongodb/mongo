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

#include "mongo/util/duration.h"
#include <iomanip>

#include "mongo/bson/mutable/document.h"
#include "mongo/config.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/json.h"
#include "mongo/db/prepare_conflict_tracker.h"
#include "mongo/db/profile_filter.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/telemetry.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/db/storage/storage_engine_feature_flags_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/metadata/impersonated_user_metadata.h"
#include "mongo/transport/service_executor.h"
#include "mongo/util/hex.h"
#include "mongo/util/log_with_sampling.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/str.h"
#include "mongo/util/system_tick_source.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

auto& oplogGetMoreStats = makeServerStatusMetric<TimerStats>("repl.network.oplogGetMoresProcessed");

BSONObj serializeDollarDbInOpDescription(boost::optional<TenantId> tenantId,
                                         const BSONObj& cmdObj) {
    auto db = cmdObj["$db"];
    if (!db) {
        return cmdObj;
    }

    auto dbName = DatabaseNameUtil::deserialize(tenantId, db.String());
    auto newCmdObj =
        cmdObj.addField(BSON("$db" << DatabaseNameUtil::serialize(dbName)).firstElement());
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
        // of the current operation.
        if (_top) {
            if (auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none)) {
                curOp->_lockStatsBase = lockerInfo->stats;
            }
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

void CurOp::reportCurrentOpForClient(OperationContext* opCtx,
                                     Client* client,
                                     bool truncateOps,
                                     bool backtraceMode,
                                     BSONObjBuilder* infoBuilder) {
    invariant(client);

    OperationContext* clientOpCtx = client->getOperationContext();

    infoBuilder->append("type", "op");

    const std::string hostName = getHostNameCachedAndPort();
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
                        opCtx->getServiceContext()->getPreciseClockSource()->now().toString());

    auto authSession = AuthorizationSession::get(client);
    // Depending on whether the authenticated user is the same user which ran the command,
    // this might be "effectiveUsers" or "runBy".
    const auto serializeAuthenticatedUsers = [&](StringData name) {
        if (authSession->isAuthenticated()) {
            BSONArrayBuilder users(infoBuilder->subarrayStart(name));
            authSession->getAuthenticatedUserName()->serializeToBSON(&users);
        }
    };

    auto maybeImpersonationData = rpc::getImpersonatedUserMetadata(clientOpCtx);
    if (maybeImpersonationData) {
        BSONArrayBuilder users(infoBuilder->subarrayStart("effectiveUsers"));

        if (maybeImpersonationData->getUser()) {
            maybeImpersonationData->getUser()->serializeToBSON(&users);
        } else if (maybeImpersonationData->getUsers()) {
            for (const auto& user : maybeImpersonationData->getUsers().get()) {
                user.serializeToBSON(&users);
            }
        }

        users.doneFast();
        serializeAuthenticatedUsers("runBy"_sd);
    } else {
        serializeAuthenticatedUsers("effectiveUsers"_sd);
    }

    if (const auto seCtx = transport::ServiceExecutorContext::get(client)) {
        infoBuilder->append("threaded"_sd, seCtx->useDedicatedThread());
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

        CurOp::get(clientOpCtx)->reportState(infoBuilder, truncateOps);
    }

#ifndef MONGO_CONFIG_USE_RAW_LATCHES
    if (auto diagnostic = DiagnosticInfo::get(*client)) {
        BSONObjBuilder waitingForLatchBuilder(infoBuilder->subobjStart("waitingForLatch"));
        waitingForLatchBuilder.append("timestamp", diagnostic->getTimestamp());
        waitingForLatchBuilder.append("captureName", diagnostic->getCaptureName());
        if (backtraceMode) {
            BSONArrayBuilder backtraceBuilder(waitingForLatchBuilder.subarrayStart("backtrace"));
            /** This branch becomes useful again with SERVER-44091
            for (const auto& frame : diagnostic->makeStackTrace().frames) {
                BSONObjBuilder backtraceObj(backtraceBuilder.subobjStart());
                backtraceObj.append("addr", unsignedHex(frame.instructionOffset));
                backtraceObj.append("path", frame.objectPath);
            }
            */
        }
    }
#endif
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

void CurOp::setOpDescription_inlock(const BSONObj& opDescription) {
    if (_nss.tenantId()) {
        _opDescription = serializeDollarDbInOpDescription(_nss.tenantId(), opDescription);
    } else {
        _opDescription = opDescription;
    }
}

void CurOp::setGenericCursor_inlock(GenericCursor gc) {
    _genericCursor = std::move(gc);
}

CurOp::~CurOp() {
    if (parent() != nullptr)
        parent()->yielded(_numYields.load());
    invariant(!_stack || this == _stack->pop());
}

void CurOp::setGenericOpRequestDetails(NamespaceString nss,
                                       const Command* command,
                                       BSONObj cmdObj,
                                       NetworkOp op) {
    // Set the _isCommand flags based on network op only. For legacy writes on mongoS, we resolve
    // them to OpMsgRequests and then pass them into the Commands path, so having a valid Command*
    // here does not guarantee that the op was issued from the client using a command protocol.
    const bool isCommand = (op == dbMsg || (op == dbQuery && nss.isCommand()));
    auto logicalOp = (command ? command->getLogicalOp() : networkOpToLogicalOp(op));

    stdx::lock_guard<Client> clientLock(*opCtx()->getClient());
    _isCommand = _debug.iscommand = isCommand;
    _logicalOp = _debug.logicalOp = logicalOp;
    _networkOp = _debug.networkOp = op;
    _opDescription = serializeDollarDbInOpDescription(nss.tenantId(), cmdObj);
    _command = command;
    _nss = std::move(nss);
}

void CurOp::setMessage_inlock(StringData message) {
    if (_progressMeter.isActive()) {
        LOGV2_ERROR(20527,
                    "Changing message from {old} to {new}",
                    "Updating message",
                    "old"_attr = redact(_message),
                    "new"_attr = redact(message));
        verify(!_progressMeter.isActive());
    }
    _message = message.toString();  // copy
}

ProgressMeter& CurOp::setProgress_inlock(StringData message,
                                         unsigned long long progressMeterTotal,
                                         int secondsBetween) {
    setMessage_inlock(message);
    _progressMeter.reset(progressMeterTotal, secondsBetween);
    _progressMeter.setName(message);
    return _progressMeter;
}

void CurOp::setNS_inlock(NamespaceString nss) {
    _nss = std::move(nss);
}

void CurOp::setNS_inlock(const DatabaseName& dbName) {
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

    // The '_start' value is initialized to 0 and gets assigned on demand the first time it gets
    // accessed. The above thread ownership requirement ensures that there will never be concurrent
    // calls to this '_start' assignment, but we use compare-exchange anyway as an additional check
    // that writes to '_start' never race.
    TickSource::Tick unassignedStart = 0;
    invariant(_start.compare_exchange_strong(unassignedStart, _tickSource->getTicks()));
    return _start.load();
}

void CurOp::done() {
    _end = _tickSource->getTicks();

    if (_cpuTimer) {
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

void CurOp::enter_inlock(NamespaceString nss, int dbProfileLevel) {
    ensureStarted();
    _nss = std::move(nss);
    raiseDbProfileLevel(dbProfileLevel);
}

void CurOp::enter_inlock(const DatabaseName& dbName, int dbProfileLevel) {
    enter_inlock(NamespaceString(dbName), dbProfileLevel);
}

void CurOp::raiseDbProfileLevel(int dbProfileLevel) {
    _dbprofile = std::max(dbProfileLevel, _dbprofile);
}

static constexpr size_t appendMaxElementSize = 50 * 1024;

bool shouldOmitDiagnosticInformation(CurOp* curop) {
    do {
        if (curop->debug().shouldOmitDiagnosticInformation) {
            return true;
        }

        curop = curop->parent();
    } while (curop != nullptr);

    return false;
}

bool CurOp::completeAndLogOperation(logv2::LogComponent component,
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
    _debug.additiveMetrics.executionTime =
        duration_cast<Microseconds>(elapsedTimeExcludingPauses());
    const auto executionTimeMillis =
        durationCount<Milliseconds>(*_debug.additiveMetrics.executionTime);

    // Do not log the slow query information if asked to omit it
    if (shouldOmitDiagnosticInformation(this)) {
        return false;
    }

    if (_debug.isReplOplogGetMore) {
        oplogGetMoreStats.recordMillis(executionTimeMillis);
    }

    bool shouldLogSlowOp, shouldProfileAtLevel1;

    if (filter) {
        bool passesFilter = filter->matches(opCtx, _debug, *this);

        shouldLogSlowOp = passesFilter;
        shouldProfileAtLevel1 = passesFilter;

    } else {
        // Log the operation if it is eligible according to the current slowMS and sampleRate
        // settings.
        bool shouldSample;
        std::tie(shouldLogSlowOp, shouldSample) = shouldLogSlowOpWithSampling(
            opCtx, component, Milliseconds(executionTimeMillis), Milliseconds(slowMs));

        shouldProfileAtLevel1 = shouldLogSlowOp && shouldSample;
    }

    if (forceLog || shouldLogSlowOp) {
        auto lockerInfo = opCtx->lockState()->getLockerInfo(_lockStatsBase);
        if (_debug.storageStats == nullptr && opCtx->lockState()->wasGlobalLockTaken() &&
            opCtx->getServiceContext()->getStorageEngine()) {
            // Do not fetch operation statistics again if we have already got them (for instance,
            // as a part of stashing the transaction).
            // Take a lock before calling into the storage engine to prevent racing against a
            // shutdown. Any operation that used a storage engine would have at-least held a
            // global lock at one point, hence we limit our lock acquisition to such operations.
            // We can get here and our lock acquisition be timed out or interrupted, log a
            // message if that happens.
            try {
                // Retrieving storage stats should not be blocked by oplog application.
                ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(
                    opCtx->lockState());
                Lock::GlobalLock lk(opCtx,
                                    MODE_IS,
                                    Date_t::now() + Milliseconds(500),
                                    Lock::InterruptBehavior::kThrow);
                _debug.storageStats =
                    opCtx->recoveryUnit()->computeOperationStatisticsSinceLastCall();
            } catch (const DBException& ex) {
                LOGV2_WARNING_OPTIONS(20526,
                                      {component},
                                      "Failed to gather storage statistics for slow operation",
                                      "opId"_attr = opCtx->getOpID(),
                                      "error"_attr = redact(ex));
            }
        }

        // Gets the time spent blocked on prepare conflicts.
        auto prepareConflictDurationMicros =
            PrepareConflictTracker::get(opCtx).getPrepareConflictDuration();
        _debug.prepareConflictDurationMillis =
            duration_cast<Milliseconds>(prepareConflictDurationMicros);

        auto operationMetricsPtr = [&]() -> ResourceConsumption::OperationMetrics* {
            auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
            if (metricsCollector.hasCollectedMetrics()) {
                return &metricsCollector.getMetrics();
            }
            return nullptr;
        }();

        logv2::DynamicAttributes attr;
        _debug.report(
            opCtx, (lockerInfo ? &lockerInfo->stats : nullptr), operationMetricsPtr, &attr);

        // TODO SERVER-67020 Ensure the ns in attr has the tenantId as the db prefix
        LOGV2_OPTIONS(51803, {component}, "Slow query", attr);

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
    return NamespaceStringUtil::serialize(_nss);
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
        case LogicalOp::opUpdate:
        case LogicalOp::opInsert:
        case LogicalOp::opDelete:
            return Command::ReadWriteType::kWrite;
        default:
            return Command::ReadWriteType::kCommand;
    }
}

namespace {

BSONObj appendCommentField(OperationContext* opCtx, const BSONObj& cmdObj) {
    return opCtx->getComment() && !cmdObj["comment"] ? cmdObj.addField(*opCtx->getComment())
                                                     : cmdObj;
}

/**
 * Appends {<name>: obj} to the provided builder.  If obj is greater than maxSize, appends a string
 * summary of obj as { <name>: { $truncated: "obj" } }. If a comment parameter is present, add it to
 * the truncation object.
 */
void appendAsObjOrString(StringData name,
                         const BSONObj& obj,
                         const boost::optional<size_t> maxSize,
                         BSONObjBuilder* builder) {
    if (!maxSize || static_cast<size_t>(obj.objsize()) <= *maxSize) {
        builder->append(name, obj);
    } else {
        // Generate an abbreviated serialization for the object, by passing false as the "full"
        // argument to obj.toString(). Remove "comment" field from the object, if present, since
        // this will be promoted to a top-level field in the output.
        std::string objToString =
            (obj.hasField("comment") ? obj.removeField("comment") : obj).toString();
        if (objToString.size() > *maxSize) {
            // objToString is still too long, so we append to the builder a truncated form
            // of objToString concatenated with "...".  Instead of creating a new string
            // temporary, mutate objToString to do this (we know that we can mutate
            // characters in objToString up to and including objToString[maxSize]).
            objToString[*maxSize - 3] = '.';
            objToString[*maxSize - 2] = '.';
            objToString[*maxSize - 1] = '.';
            LOGV2_INFO(4760300,
                       "Gathering currentOp information, operation of size {size} exceeds the size "
                       "limit of {limit} and will be truncated.",
                       "size"_attr = objToString.size(),
                       "limit"_attr = *maxSize);
        }

        StringData truncation = StringData(objToString).substr(0, *maxSize);

        // Append the truncated representation of the object to the builder. If a comment parameter
        // is present, write it to the object alongside the truncated op. This object will appear as
        // {$truncated: "{find: \"collection\", filter: {x: 1, ...", comment: "comment text" }
        BSONObjBuilder truncatedBuilder(builder->subobjStart(name));
        truncatedBuilder.append("$truncated", truncation);

        if (auto comment = obj["comment"]) {
            truncatedBuilder.append(comment);
        }

        truncatedBuilder.doneFast();
    }
}
}  // namespace

BSONObj CurOp::truncateAndSerializeGenericCursor(GenericCursor* cursor,
                                                 boost::optional<size_t> maxQuerySize) {
    // This creates a new builder to truncate the object that will go into the curOp output. In
    // order to make sure the object is not too large but not truncate the comment, we only
    // truncate the originatingCommand and not the entire cursor.
    if (maxQuerySize) {
        BSONObjBuilder tempObj;
        appendAsObjOrString(
            "truncatedObj", cursor->getOriginatingCommand().value(), maxQuerySize, &tempObj);
        auto originatingCommand = tempObj.done().getObjectField("truncatedObj");
        cursor->setOriginatingCommand(originatingCommand.getOwned());
    }
    // lsid, ns, and planSummary exist in the top level curop object, so they need to be temporarily
    // removed from the cursor object to avoid duplicating information.
    auto lsid = cursor->getLsid();
    auto ns = cursor->getNs();
    auto originalPlanSummary(cursor->getPlanSummary() ? boost::optional<std::string>(
                                                            cursor->getPlanSummary()->toString())
                                                      : boost::none);
    cursor->setLsid(boost::none);
    cursor->setNs(boost::none);
    cursor->setPlanSummary(boost::none);
    auto serialized = cursor->toBSON();
    cursor->setLsid(lsid);
    cursor->setNs(ns);
    if (originalPlanSummary) {
        cursor->setPlanSummary(StringData(*originalPlanSummary));
    }
    return serialized;
}

void CurOp::reportState(BSONObjBuilder* builder, bool truncateOps) {
    auto opCtx = this->opCtx();
    auto start = _start.load();
    if (start) {
        auto end = _end.load();
        auto elapsedTimeTotal = computeElapsedTimeTotal(start, end);
        builder->append("secs_running", durationCount<Seconds>(elapsedTimeTotal));
        builder->append("microsecs_running", durationCount<Microseconds>(elapsedTimeTotal));
    }

    builder->append("op", logicalOpToString(_logicalOp));
    builder->append("ns", NamespaceStringUtil::serialize(_nss));

    // When the currentOp command is run, it returns a single response object containing all current
    // operations; this request will fail if the response exceeds the 16MB document limit. By
    // contrast, the $currentOp aggregation stage does not have this restriction. If 'truncateOps'
    // is true, limit the size of each op to 1000 bytes. Otherwise, do not truncate.
    const boost::optional<size_t> maxQuerySize{truncateOps, 1000};
    appendAsObjOrString(
        "command", appendCommentField(opCtx, _opDescription), maxQuerySize, builder);

    switch (_debug.queryFramework) {
        case PlanExecutor::QueryFramework::kClassicOnly:
        case PlanExecutor::QueryFramework::kClassicHybrid:
            builder->append("queryFramework", "classic");
            break;
        case PlanExecutor::QueryFramework::kSBEOnly:
        case PlanExecutor::QueryFramework::kSBEHybrid:
            builder->append("queryFramework", "sbe");
            break;
        case PlanExecutor::QueryFramework::kCQF:
            builder->append("queryFramework", "cqf");
            break;
        case PlanExecutor::QueryFramework::kUnknown:
            break;
    }

    if (!_planSummary.empty()) {
        builder->append("planSummary", _planSummary);
    }

    if (_genericCursor) {
        builder->append("cursor",
                        truncateAndSerializeGenericCursor(&(*_genericCursor), maxQuerySize));
    }

    if (!_message.empty()) {
        if (_progressMeter.isActive()) {
            StringBuilder buf;
            buf << _message << " " << _progressMeter.toString();
            builder->append("msg", buf.str());
            BSONObjBuilder sub(builder->subobjStart("progress"));
            sub.appendNumber("done", (long long)_progressMeter.done());
            sub.appendNumber("total", (long long)_progressMeter.total());
            sub.done();
        } else {
            builder->append("msg", _message);
        }
    }

    if (!_failPointMessage.empty()) {
        builder->append("failpointMsg", _failPointMessage);
    }

    if (auto n = _debug.additiveMetrics.prepareReadConflicts.load(); n > 0) {
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

    // (Ignore FCV check): This feature flag is used to initialize ticketing during storage engine
    // initialization and FCV checking is ignored there, so here we also need to ignore FCV to keep
    // consistent behavior.
    if (feature_flags::gFeatureFlagDeprioritizeLowPriorityOperations
            .isEnabledAndIgnoreFCVUnsafe()) {
        auto admissionPriority = opCtx->lockState()->getAdmissionPriority();
        if (admissionPriority < AdmissionContext::Priority::kNormal) {
            builder->append("admissionPriority", toString(admissionPriority));
        }
    }

    if (auto start = _waitForWriteConcernStart.load(); start > 0) {
        auto end = _waitForWriteConcernEnd.load();
        auto elapsedTimeTotal =
            duration_cast<Microseconds>(debug().waitForWriteConcernDurationMillis);
        elapsedTimeTotal += computeElapsedTimeTotal(start, end);
        builder->append("waitForWriteConcernDurationMillis",
                        durationCount<Milliseconds>(elapsedTimeTotal));
    }
}

namespace {
StringData getProtoString(int op) {
    if (op == dbMsg) {
        return "op_msg";
    } else if (op == dbQuery) {
        return "op_query";
    }
    MONGO_UNREACHABLE;
}
}  // namespace

#define OPDEBUG_TOSTRING_HELP(x) \
    if (x >= 0)                  \
    s << " " #x ":" << (x)
#define OPDEBUG_TOSTRING_HELP_BOOL(x) \
    if (x)                            \
    s << " " #x ":" << (x)
#define OPDEBUG_TOSTRING_HELP_ATOMIC(x, y) \
    if (auto __y = y.load(); __y > 0)      \
    s << " " x ":" << (__y)
#define OPDEBUG_TOSTRING_HELP_OPTIONAL(x, y) \
    if (y)                                   \
    s << " " x ":" << (*y)

#define OPDEBUG_TOATTR_HELP(x) \
    if (x >= 0)                \
    pAttrs->add(#x, x)
#define OPDEBUG_TOATTR_HELP_BOOL(x) \
    if (x)                          \
    pAttrs->add(#x, x)
#define OPDEBUG_TOATTR_HELP_ATOMIC(x, y) \
    if (auto __y = y.load(); __y > 0)    \
    pAttrs->add(x, __y)
#define OPDEBUG_TOATTR_HELP_OPTIONAL(x, y) \
    if (y)                                 \
    pAttrs->add(x, *y)

void OpDebug::report(OperationContext* opCtx,
                     const SingleThreadedLockStats* lockStats,
                     const ResourceConsumption::OperationMetrics* operationMetrics,
                     logv2::DynamicAttributes* pAttrs) const {
    Client* client = opCtx->getClient();
    auto& curop = *CurOp::get(opCtx);
    auto flowControlStats = opCtx->lockState()->getFlowControlStats();

    if (iscommand) {
        pAttrs->add("type", "command");
    } else {
        pAttrs->add("type", networkOpToString(networkOp));
    }

    pAttrs->addDeepCopy("ns", curop.getNS());

    if (client) {
        if (auto clientMetadata = ClientMetadata::get(client)) {
            StringData appName = clientMetadata->getApplicationName();
            if (!appName.empty()) {
                pAttrs->add("appName", appName);
            }
        }
    }

    auto query = appendCommentField(opCtx, curop.opDescription());
    if (!query.isEmpty()) {
        if (iscommand) {
            const Command* curCommand = curop.getCommand();
            if (curCommand) {
                mutablebson::Document cmdToLog(query, mutablebson::Document::kInPlaceDisabled);
                curCommand->snipForLogging(&cmdToLog);
                pAttrs->add("command", redact(cmdToLog.getObject()));
            } else {
                // Should not happen but we need to handle curCommand == NULL gracefully.
                // We don't know what the request payload is intended to be, so it might be
                // sensitive, and we don't know how to redact it properly without a 'Command*'.
                // So we just don't log it at all.
                pAttrs->add("command", "unrecognized");
            }
        } else {
            pAttrs->add("command", redact(query));
        }
    }

    auto originatingCommand = curop.originatingCommand();
    if (!originatingCommand.isEmpty()) {
        pAttrs->add("originatingCommand", redact(originatingCommand));
    }

    if (!curop.getPlanSummary().empty()) {
        pAttrs->addDeepCopy("planSummary", curop.getPlanSummary().toString());
    }

    if (planningTime > Microseconds::zero()) {
        pAttrs->add("planningTimeMicros", durationCount<Microseconds>(planningTime));
    }

    if (prepareConflictDurationMillis > Milliseconds::zero()) {
        pAttrs->add("prepareConflictDuration", prepareConflictDurationMillis);
    }

    if (catalogCacheDatabaseLookupMillis > Milliseconds::zero()) {
        pAttrs->add("catalogCacheDatabaseLookupDuration", catalogCacheDatabaseLookupMillis);
    }

    if (catalogCacheCollectionLookupMillis > Milliseconds::zero()) {
        pAttrs->add("catalogCacheCollectionLookupDuration", catalogCacheCollectionLookupMillis);
    }

    if (catalogCacheIndexLookupMillis > Milliseconds::zero()) {
        pAttrs->add("catalogCacheIndexLookupDuration", catalogCacheIndexLookupMillis);
    }

    if (databaseVersionRefreshMillis > Milliseconds::zero()) {
        pAttrs->add("databaseVersionRefreshDuration", databaseVersionRefreshMillis);
    }

    if (placementVersionRefreshMillis > Milliseconds::zero()) {
        pAttrs->add("placementVersionRefreshDuration", placementVersionRefreshMillis);
    }

    if (totalOplogSlotDurationMicros > Microseconds::zero()) {
        pAttrs->add("totalOplogSlotDuration", totalOplogSlotDurationMicros);
    }

    if (dataThroughputLastSecond) {
        pAttrs->add("dataThroughputLastSecondMBperSec", *dataThroughputLastSecond);
    }

    if (dataThroughputAverage) {
        pAttrs->add("dataThroughputAverageMBPerSec", *dataThroughputAverage);
    }

    if (!resolvedViews.empty()) {
        pAttrs->add("resolvedViews", getResolvedViewsInfo());
    }

    OPDEBUG_TOATTR_HELP(nShards);
    OPDEBUG_TOATTR_HELP(cursorid);
    if (mongotCursorId) {
        pAttrs->add("mongot", makeMongotDebugStatsObject());
    }
    OPDEBUG_TOATTR_HELP_BOOL(exhaust);

    OPDEBUG_TOATTR_HELP_OPTIONAL("keysExamined", additiveMetrics.keysExamined);
    OPDEBUG_TOATTR_HELP_OPTIONAL("docsExamined", additiveMetrics.docsExamined);
    OPDEBUG_TOATTR_HELP_BOOL(hasSortStage);
    OPDEBUG_TOATTR_HELP_BOOL(usedDisk);
    OPDEBUG_TOATTR_HELP_BOOL(fromMultiPlanner);
    OPDEBUG_TOATTR_HELP_BOOL(fromPlanCache);
    if (replanReason) {
        bool replanned = true;
        OPDEBUG_TOATTR_HELP_BOOL(replanned);
        pAttrs->add("replanReason", redact(*replanReason));
    }
    OPDEBUG_TOATTR_HELP_OPTIONAL("nMatched", additiveMetrics.nMatched);
    OPDEBUG_TOATTR_HELP_OPTIONAL("nBatches", additiveMetrics.nBatches);
    OPDEBUG_TOATTR_HELP_OPTIONAL("nModified", additiveMetrics.nModified);
    OPDEBUG_TOATTR_HELP_OPTIONAL("ninserted", additiveMetrics.ninserted);
    OPDEBUG_TOATTR_HELP_OPTIONAL("ndeleted", additiveMetrics.ndeleted);
    OPDEBUG_TOATTR_HELP_OPTIONAL("nUpserted", additiveMetrics.nUpserted);
    OPDEBUG_TOATTR_HELP_BOOL(cursorExhausted);

    OPDEBUG_TOATTR_HELP_OPTIONAL("keysInserted", additiveMetrics.keysInserted);
    OPDEBUG_TOATTR_HELP_OPTIONAL("keysDeleted", additiveMetrics.keysDeleted);
    OPDEBUG_TOATTR_HELP_ATOMIC("prepareReadConflicts", additiveMetrics.prepareReadConflicts);
    OPDEBUG_TOATTR_HELP_ATOMIC("writeConflicts", additiveMetrics.writeConflicts);
    OPDEBUG_TOATTR_HELP_ATOMIC("temporarilyUnavailableErrors",
                               additiveMetrics.temporarilyUnavailableErrors);

    pAttrs->add("numYields", curop.numYields());
    OPDEBUG_TOATTR_HELP_OPTIONAL("nreturned", additiveMetrics.nreturned);

    if (queryHash) {
        pAttrs->addDeepCopy("queryHash", zeroPaddedHex(*queryHash));
    }
    if (planCacheKey) {
        pAttrs->addDeepCopy("planCacheKey", zeroPaddedHex(*planCacheKey));
    }

    switch (queryFramework) {
        case PlanExecutor::QueryFramework::kClassicOnly:
        case PlanExecutor::QueryFramework::kClassicHybrid:
            pAttrs->add("queryFramework", "classic");
            break;
        case PlanExecutor::QueryFramework::kSBEOnly:
        case PlanExecutor::QueryFramework::kSBEHybrid:
            pAttrs->add("queryFramework", "sbe");
            break;
        case PlanExecutor::QueryFramework::kCQF:
            pAttrs->add("queryFramework", "cqf");
            break;
        case PlanExecutor::QueryFramework::kUnknown:
            break;
    }

    if (!errInfo.isOK()) {
        pAttrs->add("ok", 0);
        if (!errInfo.reason().empty()) {
            pAttrs->add("errMsg", redact(errInfo.reason()));
        }
        pAttrs->addDeepCopy("errName", errInfo.codeString());
        pAttrs->add("errCode", static_cast<int>(errInfo.code()));
    }

    if (responseLength > 0) {
        pAttrs->add("reslen", responseLength);
    }

    // (Ignore FCV check): This feature flag is used to initialize ticketing during storage engine
    // initialization and FCV checking is ignored there, so here we also need to ignore FCV to keep
    // consistent behavior.
    if (feature_flags::gFeatureFlagDeprioritizeLowPriorityOperations
            .isEnabledAndIgnoreFCVUnsafe()) {
        auto admissionPriority = opCtx->lockState()->getAdmissionPriority();
        if (admissionPriority < AdmissionContext::Priority::kNormal) {
            pAttrs->add("admissionPriority", admissionPriority);
        }
    }

    if (lockStats) {
        BSONObjBuilder locks;
        lockStats->report(&locks);
        pAttrs->add("locks", locks.obj());
    }

    auto userAcquisitionStats = curop.getReadOnlyUserAcquisitionStats();
    if (userAcquisitionStats->shouldUserCacheAcquisitionStatsReport()) {
        BSONObjBuilder userCacheAcquisitionStatsBuilder;
        userAcquisitionStats->userCacheAcquisitionStatsReport(
            &userCacheAcquisitionStatsBuilder, opCtx->getServiceContext()->getTickSource());
        pAttrs->add("authorization", userCacheAcquisitionStatsBuilder.obj());
    }

    if (userAcquisitionStats->shouldLDAPOperationStatsReport()) {
        BSONObjBuilder ldapOperationStatsBuilder;
        userAcquisitionStats->ldapOperationStatsReport(&ldapOperationStatsBuilder,
                                                       opCtx->getServiceContext()->getTickSource());
        pAttrs->add("LDAPOperations", ldapOperationStatsBuilder.obj());
    }

    BSONObj flowControlObj = makeFlowControlObject(flowControlStats);
    if (flowControlObj.nFields() > 0) {
        pAttrs->add("flowControl", flowControlObj);
    }

    {
        const auto& readConcern = repl::ReadConcernArgs::get(opCtx);
        if (readConcern.isSpecified()) {
            pAttrs->add("readConcern", readConcern.toBSONInner());
        }
    }

    if (writeConcern && !writeConcern->usedDefaultConstructedWC) {
        pAttrs->add("writeConcern", writeConcern->toBSON());
    }

    if (waitForWriteConcernDurationMillis > Milliseconds::zero()) {
        pAttrs->add("waitForWriteConcernDuration", waitForWriteConcernDurationMillis);
    }

    if (storageStats) {
        pAttrs->add("storage", storageStats->toBSON());
    }

    if (operationMetrics) {
        BSONObjBuilder builder;
        operationMetrics->toBsonNonZeroFields(&builder);
        pAttrs->add("operationMetrics", builder.obj());
    }

    if (cpuTime > Nanoseconds::zero()) {
        pAttrs->add("cpuNanos", durationCount<Nanoseconds>(cpuTime));
    }

    if (client && client->session()) {
        pAttrs->add("remote", client->session()->remote());
    }

    if (iscommand) {
        pAttrs->add("protocol", getProtoString(networkOp));
    }

    if (const auto& invocation = CommandInvocation::get(opCtx);
        invocation && invocation->isMirrored()) {
        const bool mirrored = true;
        OPDEBUG_TOATTR_HELP_BOOL(mirrored);
    }

    if (remoteOpWaitTime) {
        pAttrs->add("remoteOpWaitMillis", durationCount<Milliseconds>(*remoteOpWaitTime));
    }

    // durationMillis should always be present for any operation
    pAttrs->add(
        "durationMillis",
        durationCount<Milliseconds>(additiveMetrics.executionTime.value_or(Microseconds{0})));
}

#define OPDEBUG_APPEND_NUMBER2(b, x, y) \
    if (y != -1)                        \
    (b).appendNumber(x, (y))
#define OPDEBUG_APPEND_NUMBER(b, x) OPDEBUG_APPEND_NUMBER2(b, #x, x)

#define OPDEBUG_APPEND_BOOL2(b, x, y) \
    if (y)                            \
    (b).appendBool(x, (y))
#define OPDEBUG_APPEND_BOOL(b, x) OPDEBUG_APPEND_BOOL2(b, #x, x)

#define OPDEBUG_APPEND_ATOMIC(b, x, y) \
    if (auto __y = y.load(); __y > 0)  \
    (b).appendNumber(x, __y)
#define OPDEBUG_APPEND_OPTIONAL(b, x, y) \
    if (y)                               \
    (b).appendNumber(x, (*y))

void OpDebug::append(OperationContext* opCtx,
                     const SingleThreadedLockStats& lockStats,
                     FlowControlTicketholder::CurOp flowControlStats,
                     BSONObjBuilder& b) const {
    auto& curop = *CurOp::get(opCtx);

    b.append("op", logicalOpToString(logicalOp));

    NamespaceString nss = NamespaceString(curop.getNS());
    b.append("ns", nss.ns());

    appendAsObjOrString(
        "command", appendCommentField(opCtx, curop.opDescription()), appendMaxElementSize, &b);

    auto originatingCommand = curop.originatingCommand();
    if (!originatingCommand.isEmpty()) {
        appendAsObjOrString("originatingCommand", originatingCommand, appendMaxElementSize, &b);
    }

    if (!resolvedViews.empty()) {
        appendResolvedViewsInfo(b);
    }

    OPDEBUG_APPEND_NUMBER(b, nShards);
    OPDEBUG_APPEND_NUMBER(b, cursorid);
    if (mongotCursorId) {
        b.append("mongot", makeMongotDebugStatsObject());
    }
    OPDEBUG_APPEND_BOOL(b, exhaust);

    OPDEBUG_APPEND_OPTIONAL(b, "keysExamined", additiveMetrics.keysExamined);
    OPDEBUG_APPEND_OPTIONAL(b, "docsExamined", additiveMetrics.docsExamined);
    OPDEBUG_APPEND_BOOL(b, hasSortStage);
    OPDEBUG_APPEND_BOOL(b, usedDisk);
    OPDEBUG_APPEND_BOOL(b, fromMultiPlanner);
    OPDEBUG_APPEND_BOOL(b, fromPlanCache);
    if (replanReason) {
        bool replanned = true;
        OPDEBUG_APPEND_BOOL(b, replanned);
        b.append("replanReason", *replanReason);
    }
    OPDEBUG_APPEND_OPTIONAL(b, "nMatched", additiveMetrics.nMatched);
    OPDEBUG_APPEND_OPTIONAL(b, "nBatches", additiveMetrics.nBatches);
    OPDEBUG_APPEND_OPTIONAL(b, "nModified", additiveMetrics.nModified);
    OPDEBUG_APPEND_OPTIONAL(b, "ninserted", additiveMetrics.ninserted);
    OPDEBUG_APPEND_OPTIONAL(b, "ndeleted", additiveMetrics.ndeleted);
    OPDEBUG_APPEND_OPTIONAL(b, "nUpserted", additiveMetrics.nUpserted);
    OPDEBUG_APPEND_BOOL(b, cursorExhausted);

    OPDEBUG_APPEND_OPTIONAL(b, "keysInserted", additiveMetrics.keysInserted);
    OPDEBUG_APPEND_OPTIONAL(b, "keysDeleted", additiveMetrics.keysDeleted);
    OPDEBUG_APPEND_ATOMIC(b, "prepareReadConflicts", additiveMetrics.prepareReadConflicts);
    OPDEBUG_APPEND_ATOMIC(b, "writeConflicts", additiveMetrics.writeConflicts);
    OPDEBUG_APPEND_ATOMIC(
        b, "temporarilyUnavailableErrors", additiveMetrics.temporarilyUnavailableErrors);

    OPDEBUG_APPEND_OPTIONAL(b, "dataThroughputLastSecond", dataThroughputLastSecond);
    OPDEBUG_APPEND_OPTIONAL(b, "dataThroughputAverage", dataThroughputAverage);

    b.appendNumber("numYield", curop.numYields());
    OPDEBUG_APPEND_OPTIONAL(b, "nreturned", additiveMetrics.nreturned);

    if (queryHash) {
        b.append("queryHash", zeroPaddedHex(*queryHash));
    }
    if (planCacheKey) {
        b.append("planCacheKey", zeroPaddedHex(*planCacheKey));
    }

    switch (queryFramework) {
        case PlanExecutor::QueryFramework::kClassicOnly:
        case PlanExecutor::QueryFramework::kClassicHybrid:
            b.append("queryFramework", "classic");
            break;
        case PlanExecutor::QueryFramework::kSBEOnly:
        case PlanExecutor::QueryFramework::kSBEHybrid:
            b.append("queryFramework", "sbe");
            break;
        case PlanExecutor::QueryFramework::kCQF:
            b.append("queryFramework", "cqf");
            break;
        case PlanExecutor::QueryFramework::kUnknown:
            break;
    }

    {
        BSONObjBuilder locks(b.subobjStart("locks"));
        lockStats.report(&locks);
    }

    {
        auto userAcquisitionStats = curop.getReadOnlyUserAcquisitionStats();
        if (userAcquisitionStats->shouldUserCacheAcquisitionStatsReport()) {
            BSONObjBuilder userCacheAcquisitionStatsBuilder(b.subobjStart("authorization"));
            userAcquisitionStats->userCacheAcquisitionStatsReport(
                &userCacheAcquisitionStatsBuilder, opCtx->getServiceContext()->getTickSource());
        }

        if (userAcquisitionStats->shouldLDAPOperationStatsReport()) {
            BSONObjBuilder ldapOperationStatsBuilder;
            userAcquisitionStats->ldapOperationStatsReport(
                &ldapOperationStatsBuilder, opCtx->getServiceContext()->getTickSource());
        }
    }

    {
        BSONObj flowControlMetrics = makeFlowControlObject(flowControlStats);
        BSONObjBuilder flowControlBuilder(b.subobjStart("flowControl"));
        flowControlBuilder.appendElements(flowControlMetrics);
    }

    {
        const auto& readConcern = repl::ReadConcernArgs::get(opCtx);
        if (readConcern.isSpecified()) {
            readConcern.appendInfo(&b);
        }
    }

    if (writeConcern && !writeConcern->usedDefaultConstructedWC) {
        b.append("writeConcern", writeConcern->toBSON());
    }

    if (storageStats) {
        b.append("storage", storageStats->toBSON());
    }

    if (!errInfo.isOK()) {
        b.appendNumber("ok", 0.0);
        if (!errInfo.reason().empty()) {
            b.append("errMsg", errInfo.reason());
        }
        b.append("errName", ErrorCodes::errorString(errInfo.code()));
        b.append("errCode", errInfo.code());
    }

    OPDEBUG_APPEND_NUMBER(b, responseLength);
    if (iscommand) {
        b.append("protocol", getProtoString(networkOp));
    }

    if (remoteOpWaitTime) {
        b.append("remoteOpWaitMillis", durationCount<Milliseconds>(*remoteOpWaitTime));
    }

    if (cpuTime > Nanoseconds::zero()) {
        b.appendNumber("cpuNanos", durationCount<Nanoseconds>(cpuTime));
    }

    // millis should always be present for any operation
    b.appendNumber(
        "millis",
        durationCount<Milliseconds>(additiveMetrics.executionTime.value_or(Microseconds{0})));

    if (!curop.getPlanSummary().empty()) {
        b.append("planSummary", curop.getPlanSummary());
    }

    if (planningTime > Microseconds::zero()) {
        b.appendNumber("planningTimeMicros", durationCount<Microseconds>(planningTime));
    }

    if (totalOplogSlotDurationMicros > Microseconds::zero()) {
        b.appendNumber("totalOplogSlotDurationMicros",
                       durationCount<Microseconds>(totalOplogSlotDurationMicros));
    }

    if (!execStats.isEmpty()) {
        b.append("execStats", std::move(execStats));
    }
}

void OpDebug::appendUserInfo(const CurOp& c,
                             BSONObjBuilder& builder,
                             AuthorizationSession* authSession) {
    std::string opdb(nsToDatabase(c.getNS()));

    BSONArrayBuilder allUsers(builder.subarrayStart("allUsers"));
    auto name = authSession->getAuthenticatedUserName();
    if (name) {
        name->serializeToBSON(&allUsers);
    }
    allUsers.doneFast();

    builder.append("user", name ? name->getDisplayName() : "");
}

std::function<BSONObj(ProfileFilter::Args)> OpDebug::appendStaged(StringSet requestedFields,
                                                                  bool needWholeDocument) {
    // This function is analogous to OpDebug::append. The main difference is that append() does
    // the work of building BSON right away, while appendStaged() stages the work to be done later.
    // It returns a std::function that builds BSON when called.

    // The other difference is that appendStaged can avoid building BSON for unneeded fields.
    // requestedFields is a set of top-level field names; any fields beyond this list may be
    // omitted. This also lets us uassert if the caller asks for an unsupported field.

    // Each piece of the result is a function that appends to a BSONObjBuilder.
    // Before returning, we encapsulate the result in a simpler function that returns a BSONObj.
    using Piece = std::function<void(ProfileFilter::Args, BSONObjBuilder&)>;
    std::vector<Piece> pieces;

    // For convenience, the callback that handles each field gets the fieldName as an extra arg.
    using Callback = std::function<void(const char*, ProfileFilter::Args, BSONObjBuilder&)>;

    // Helper to check for the presence of a field in the StringSet, and remove it.
    // At the end of this method, anything left in the StringSet is a field we don't know
    // how to handle.
    auto needs = [&](const char* fieldName) {
        bool val = needWholeDocument || requestedFields.count(fieldName) > 0;
        requestedFields.erase(fieldName);
        return val;
    };
    auto addIfNeeded = [&](const char* fieldName, Callback cb) {
        if (needs(fieldName)) {
            pieces.push_back([fieldName = fieldName, cb = std::move(cb)](auto args, auto& b) {
                cb(fieldName, args, b);
            });
        }
    };

    addIfNeeded("ts", [](auto field, auto args, auto& b) { b.append(field, jsTime()); });
    addIfNeeded("client", [](auto field, auto args, auto& b) {
        b.append(field, args.opCtx->getClient()->clientAddress());
    });
    addIfNeeded("appName", [](auto field, auto args, auto& b) {
        if (auto clientMetadata = ClientMetadata::get(args.opCtx->getClient())) {
            auto appName = clientMetadata->getApplicationName();
            if (!appName.empty()) {
                b.append(field, appName);
            }
        }
    });
    bool needsAllUsers = needs("allUsers");
    bool needsUser = needs("user");
    if (needsAllUsers || needsUser) {
        pieces.push_back([](auto args, auto& b) {
            AuthorizationSession* authSession = AuthorizationSession::get(args.opCtx->getClient());
            appendUserInfo(args.curop, b, authSession);
        });
    }

    addIfNeeded("op", [](auto field, auto args, auto& b) {
        b.append(field, logicalOpToString(args.op.logicalOp));
    });
    addIfNeeded("ns", [](auto field, auto args, auto& b) {
        b.append(field, NamespaceString(args.curop.getNS()).ns());
    });

    addIfNeeded("command", [](auto field, auto args, auto& b) {
        appendAsObjOrString(field,
                            appendCommentField(args.opCtx, args.curop.opDescription()),
                            appendMaxElementSize,
                            &b);
    });

    addIfNeeded("originatingCommand", [](auto field, auto args, auto& b) {
        auto originatingCommand = args.curop.originatingCommand();
        if (!originatingCommand.isEmpty()) {
            appendAsObjOrString(field, originatingCommand, appendMaxElementSize, &b);
        }
    });

    addIfNeeded("nShards", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_NUMBER2(b, field, args.op.nShards);
    });
    addIfNeeded("cursorid", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_NUMBER2(b, field, args.op.cursorid);
    });
    addIfNeeded("mongot", [](auto field, auto args, auto& b) {
        if (args.op.mongotCursorId) {
            b.append(field, args.op.makeMongotDebugStatsObject());
        }
    });
    addIfNeeded("exhaust", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_BOOL2(b, field, args.op.exhaust);
    });

    addIfNeeded("keysExamined", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.keysExamined);
    });
    addIfNeeded("docsExamined", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.docsExamined);
    });
    addIfNeeded("hasSortStage", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_BOOL2(b, field, args.op.hasSortStage);
    });
    addIfNeeded("usedDisk", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_BOOL2(b, field, args.op.usedDisk);
    });
    addIfNeeded("fromMultiPlanner", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_BOOL2(b, field, args.op.fromMultiPlanner);
    });
    addIfNeeded("fromPlanCache", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_BOOL2(b, field, args.op.fromPlanCache);
    });
    addIfNeeded("replanned", [](auto field, auto args, auto& b) {
        if (args.op.replanReason) {
            OPDEBUG_APPEND_BOOL2(b, field, true);
        }
    });
    addIfNeeded("replanReason", [](auto field, auto args, auto& b) {
        if (args.op.replanReason) {
            b.append(field, *args.op.replanReason);
        }
    });
    addIfNeeded("nMatched", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.nMatched);
    });
    addIfNeeded("nBatches", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.nBatches);
    });
    addIfNeeded("nModified", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.nModified);
    });
    addIfNeeded("ninserted", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.ninserted);
    });
    addIfNeeded("ndeleted", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.ndeleted);
    });
    addIfNeeded("nUpserted", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.nUpserted);
    });
    addIfNeeded("cursorExhausted", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_BOOL2(b, field, args.op.cursorExhausted);
    });

    addIfNeeded("keysInserted", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.keysInserted);
    });
    addIfNeeded("keysDeleted", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.keysDeleted);
    });
    addIfNeeded("prepareReadConflicts", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_ATOMIC(b, field, args.op.additiveMetrics.prepareReadConflicts);
    });
    addIfNeeded("writeConflicts", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_ATOMIC(b, field, args.op.additiveMetrics.writeConflicts);
    });
    addIfNeeded("temporarilyUnavailableErrors", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_ATOMIC(b, field, args.op.additiveMetrics.temporarilyUnavailableErrors);
    });

    addIfNeeded("dataThroughputLastSecond", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.dataThroughputLastSecond);
    });
    addIfNeeded("dataThroughputAverage", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.dataThroughputAverage);
    });

    addIfNeeded("numYield", [](auto field, auto args, auto& b) {
        b.appendNumber(field, args.curop.numYields());
    });
    addIfNeeded("nreturned", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.nreturned);
    });

    addIfNeeded("queryHash", [](auto field, auto args, auto& b) {
        if (args.op.queryHash) {
            b.append(field, zeroPaddedHex(*args.op.queryHash));
        }
    });
    addIfNeeded("planCacheKey", [](auto field, auto args, auto& b) {
        if (args.op.planCacheKey) {
            b.append(field, zeroPaddedHex(*args.op.planCacheKey));
        }
    });

    addIfNeeded("queryFramework", [](auto field, auto args, auto& b) {
        switch (args.op.queryFramework) {
            case PlanExecutor::QueryFramework::kClassicOnly:
            case PlanExecutor::QueryFramework::kClassicHybrid:
                b.append("queryFramework", "classic");
                break;
            case PlanExecutor::QueryFramework::kSBEOnly:
            case PlanExecutor::QueryFramework::kSBEHybrid:
                b.append("queryFramework", "sbe");
                break;
            case PlanExecutor::QueryFramework::kCQF:
                b.append("queryFramework", "cqf");
                break;
            case PlanExecutor::QueryFramework::kUnknown:
                break;
        }
    });

    addIfNeeded("locks", [](auto field, auto args, auto& b) {
        if (auto lockerInfo =
                args.opCtx->lockState()->getLockerInfo(args.curop.getLockStatsBase())) {
            BSONObjBuilder locks(b.subobjStart(field));
            lockerInfo->stats.report(&locks);
        }
    });

    addIfNeeded("authorization", [](auto field, auto args, auto& b) {
        auto userAcquisitionStats = args.curop.getReadOnlyUserAcquisitionStats();
        if (userAcquisitionStats->shouldUserCacheAcquisitionStatsReport()) {
            BSONObjBuilder userCacheAcquisitionStatsBuilder(b.subobjStart(field));
            userAcquisitionStats->userCacheAcquisitionStatsReport(
                &userCacheAcquisitionStatsBuilder,
                args.opCtx->getServiceContext()->getTickSource());
        }

        if (userAcquisitionStats->shouldLDAPOperationStatsReport()) {
            BSONObjBuilder ldapOperationStatsBuilder(b.subobjStart(field));
            userAcquisitionStats->ldapOperationStatsReport(
                &ldapOperationStatsBuilder, args.opCtx->getServiceContext()->getTickSource());
        }
    });

    addIfNeeded("flowControl", [](auto field, auto args, auto& b) {
        BSONObj flowControlMetrics =
            makeFlowControlObject(args.opCtx->lockState()->getFlowControlStats());
        BSONObjBuilder flowControlBuilder(b.subobjStart(field));
        flowControlBuilder.appendElements(flowControlMetrics);
    });

    addIfNeeded("writeConcern", [](auto field, auto args, auto& b) {
        if (args.op.writeConcern && !args.op.writeConcern->usedDefaultConstructedWC) {
            b.append(field, args.op.writeConcern->toBSON());
        }
    });

    addIfNeeded("storage", [](auto field, auto args, auto& b) {
        if (args.op.storageStats) {
            b.append(field, args.op.storageStats->toBSON());
        }
    });

    // Don't short-circuit: call needs() for every supported field, so that at the end we can
    // uassert that no unsupported fields were requested.
    bool needsOk = needs("ok");
    bool needsErrMsg = needs("errMsg");
    bool needsErrName = needs("errName");
    bool needsErrCode = needs("errCode");
    if (needsOk || needsErrMsg || needsErrName || needsErrCode) {
        pieces.push_back([](auto args, auto& b) {
            if (!args.op.errInfo.isOK()) {
                b.appendNumber("ok", 0.0);
                if (!args.op.errInfo.reason().empty()) {
                    b.append("errMsg", args.op.errInfo.reason());
                }
                b.append("errName", ErrorCodes::errorString(args.op.errInfo.code()));
                b.append("errCode", args.op.errInfo.code());
            }
        });
    }

    addIfNeeded("responseLength", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_NUMBER2(b, field, args.op.responseLength);
    });

    addIfNeeded("protocol", [](auto field, auto args, auto& b) {
        if (args.op.iscommand) {
            b.append(field, getProtoString(args.op.networkOp));
        }
    });

    addIfNeeded("remoteOpWaitMillis", [](auto field, auto args, auto& b) {
        if (args.op.remoteOpWaitTime) {
            b.append(field, durationCount<Milliseconds>(*args.op.remoteOpWaitTime));
        }
    });

    addIfNeeded("cpuNanos", [](auto field, auto args, auto& b) {
        if (args.op.cpuTime > Nanoseconds::zero()) {
            b.appendNumber(field, durationCount<Nanoseconds>(args.op.cpuTime));
        }
    });

    // millis and durationMillis are the same thing. This is one of the few inconsistencies between
    // the profiler (OpDebug::append) and the log file (OpDebug::report), so for the profile filter
    // we support both names.
    addIfNeeded("millis", [](auto field, auto args, auto& b) {
        b.appendNumber(field,
                       durationCount<Milliseconds>(
                           args.op.additiveMetrics.executionTime.value_or(Microseconds{0})));
    });
    addIfNeeded("durationMillis", [](auto field, auto args, auto& b) {
        b.appendNumber(field,
                       durationCount<Milliseconds>(
                           args.op.additiveMetrics.executionTime.value_or(Microseconds{0})));
    });

    addIfNeeded("planSummary", [](auto field, auto args, auto& b) {
        if (!args.curop.getPlanSummary().empty()) {
            b.append(field, args.curop.getPlanSummary());
        }
    });

    addIfNeeded("planningTimeMicros", [](auto field, auto args, auto& b) {
        b.appendNumber(field, durationCount<Microseconds>(args.op.planningTime));
    });

    addIfNeeded("totalOplogSlotDurationMicros", [](auto field, auto args, auto& b) {
        if (args.op.totalOplogSlotDurationMicros > Nanoseconds::zero()) {
            b.appendNumber(field,
                           durationCount<Microseconds>(args.op.totalOplogSlotDurationMicros));
        }
    });

    addIfNeeded("execStats", [](auto field, auto args, auto& b) {
        if (!args.op.execStats.isEmpty()) {
            b.append(field, args.op.execStats);
        }
    });

    addIfNeeded("operationMetrics", [](auto field, auto args, auto& b) {
        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(args.opCtx);
        if (metricsCollector.hasCollectedMetrics()) {
            BSONObjBuilder metricsBuilder(b.subobjStart(field));
            metricsCollector.getMetrics().toBson(&metricsBuilder);
        }
    });

    if (!requestedFields.empty()) {
        std::stringstream ss;
        ss << "No such field (or fields) available for profile filter";
        auto sep = ": ";
        for (auto&& s : requestedFields) {
            ss << sep << s;
            sep = ", ";
        }
        uasserted(4910200, ss.str());
    }

    return [pieces = std::move(pieces)](ProfileFilter::Args args) {
        BSONObjBuilder bob;
        for (const auto& piece : pieces) {
            piece(args, bob);
        }
        return bob.obj();
    };
}

void OpDebug::setPlanSummaryMetrics(const PlanSummaryStats& planSummaryStats) {
    additiveMetrics.keysExamined = planSummaryStats.totalKeysExamined;
    additiveMetrics.docsExamined = planSummaryStats.totalDocsExamined;
    hasSortStage = planSummaryStats.hasSortStage;
    usedDisk = planSummaryStats.usedDisk;
    sortSpills = planSummaryStats.sortSpills;
    sortTotalDataSizeBytes = planSummaryStats.sortTotalDataSizeBytes;
    keysSorted = planSummaryStats.keysSorted;
    fromMultiPlanner = planSummaryStats.fromMultiPlanner;
    fromPlanCache = planSummaryStats.fromPlanCache;
    replanReason = planSummaryStats.replanReason;
}

BSONObj OpDebug::makeFlowControlObject(FlowControlTicketholder::CurOp stats) {
    BSONObjBuilder builder;
    if (stats.ticketsAcquired > 0) {
        builder.append("acquireCount", stats.ticketsAcquired);
    }

    if (stats.acquireWaitCount > 0) {
        builder.append("acquireWaitCount", stats.acquireWaitCount);
    }

    if (stats.timeAcquiringMicros > 0) {
        builder.append("timeAcquiringMicros", stats.timeAcquiringMicros);
    }

    return builder.obj();
}

BSONObj OpDebug::makeMongotDebugStatsObject() const {
    BSONObjBuilder cursorBuilder;
    invariant(mongotCursorId);
    cursorBuilder.append("cursorid", mongotCursorId.value());
    if (msWaitingForMongot) {
        cursorBuilder.append("timeWaitingMillis", msWaitingForMongot.value());
    }
    cursorBuilder.append("batchNum", mongotBatchNum);
    return cursorBuilder.obj();
}

void OpDebug::addResolvedViews(const std::vector<NamespaceString>& namespaces,
                               const std::vector<BSONObj>& pipeline) {
    if (namespaces.empty())
        return;

    if (resolvedViews.find(namespaces.front()) == resolvedViews.end()) {
        resolvedViews[namespaces.front()] = std::make_pair(namespaces, pipeline);
    }
}

static void appendResolvedViewsInfoImpl(
    BSONArrayBuilder& resolvedViewsArr,
    const std::map<NamespaceString, std::pair<std::vector<NamespaceString>, std::vector<BSONObj>>>&
        resolvedViews) {
    for (const auto& kv : resolvedViews) {
        const NamespaceString& viewNss = kv.first;
        const std::vector<NamespaceString>& dependencies = kv.second.first;
        const std::vector<BSONObj>& pipeline = kv.second.second;

        BSONObjBuilder aView;
        aView.append("viewNamespace", viewNss.ns());

        BSONArrayBuilder dependenciesArr(aView.subarrayStart("dependencyChain"));
        for (const auto& nss : dependencies) {
            dependenciesArr.append(nss.coll().toString());
        }
        dependenciesArr.doneFast();

        BSONArrayBuilder pipelineArr(aView.subarrayStart("resolvedPipeline"));
        for (const auto& stage : pipeline) {
            pipelineArr.append(stage);
        }
        pipelineArr.doneFast();

        resolvedViewsArr.append(redact(aView.done()));
    }
}

BSONArray OpDebug::getResolvedViewsInfo() const {
    BSONArrayBuilder resolvedViewsArr;
    appendResolvedViewsInfoImpl(resolvedViewsArr, this->resolvedViews);
    return resolvedViewsArr.arr();
}

void OpDebug::appendResolvedViewsInfo(BSONObjBuilder& builder) const {
    BSONArrayBuilder resolvedViewsArr(builder.subarrayStart("resolvedViews"));
    appendResolvedViewsInfoImpl(resolvedViewsArr, this->resolvedViews);
    resolvedViewsArr.doneFast();
}

namespace {

/**
 * Adds two boost::optionals of the same type with an operator+() together. Returns boost::none if
 * both 'lhs' and 'rhs' are uninitialized, or the sum of 'lhs' and 'rhs' if they are both
 * initialized. Returns 'lhs' if only 'rhs' is uninitialized and vice-versa.
 */
template <typename T>
boost::optional<T> addOptionals(const boost::optional<T>& lhs, const boost::optional<T>& rhs) {
    if (!rhs) {
        return lhs;
    }
    return lhs ? (*lhs + *rhs) : rhs;
}
}  // namespace

void OpDebug::AdditiveMetrics::add(const AdditiveMetrics& otherMetrics) {
    keysExamined = addOptionals(keysExamined, otherMetrics.keysExamined);
    docsExamined = addOptionals(docsExamined, otherMetrics.docsExamined);
    nMatched = addOptionals(nMatched, otherMetrics.nMatched);
    nreturned = addOptionals(nreturned, otherMetrics.nreturned);
    nBatches = addOptionals(nBatches, otherMetrics.nBatches);
    nModified = addOptionals(nModified, otherMetrics.nModified);
    ninserted = addOptionals(ninserted, otherMetrics.ninserted);
    ndeleted = addOptionals(ndeleted, otherMetrics.ndeleted);
    nUpserted = addOptionals(nUpserted, otherMetrics.nUpserted);
    keysInserted = addOptionals(keysInserted, otherMetrics.keysInserted);
    keysDeleted = addOptionals(keysDeleted, otherMetrics.keysDeleted);
    prepareReadConflicts.fetchAndAdd(otherMetrics.prepareReadConflicts.load());
    writeConflicts.fetchAndAdd(otherMetrics.writeConflicts.load());
    temporarilyUnavailableErrors.fetchAndAdd(otherMetrics.temporarilyUnavailableErrors.load());
    executionTime = addOptionals(executionTime, otherMetrics.executionTime);
}

void OpDebug::AdditiveMetrics::reset() {
    keysExamined = boost::none;
    docsExamined = boost::none;
    nMatched = boost::none;
    nreturned = boost::none;
    nBatches = boost::none;
    nModified = boost::none;
    ninserted = boost::none;
    ndeleted = boost::none;
    nUpserted = boost::none;
    keysInserted = boost::none;
    keysDeleted = boost::none;
    prepareReadConflicts.store(0);
    writeConflicts.store(0);
    temporarilyUnavailableErrors.store(0);
    executionTime = boost::none;
}

bool OpDebug::AdditiveMetrics::equals(const AdditiveMetrics& otherMetrics) const {
    return keysExamined == otherMetrics.keysExamined && docsExamined == otherMetrics.docsExamined &&
        nMatched == otherMetrics.nMatched && nreturned == otherMetrics.nreturned &&
        nBatches == otherMetrics.nBatches && nModified == otherMetrics.nModified &&
        ninserted == otherMetrics.ninserted && ndeleted == otherMetrics.ndeleted &&
        nUpserted == otherMetrics.nUpserted && keysInserted == otherMetrics.keysInserted &&
        keysDeleted == otherMetrics.keysDeleted &&
        prepareReadConflicts.load() == otherMetrics.prepareReadConflicts.load() &&
        writeConflicts.load() == otherMetrics.writeConflicts.load() &&
        temporarilyUnavailableErrors.load() == otherMetrics.temporarilyUnavailableErrors.load() &&
        executionTime == otherMetrics.executionTime;
}

void OpDebug::AdditiveMetrics::incrementWriteConflicts(long long n) {
    writeConflicts.fetchAndAdd(n);
}

void OpDebug::AdditiveMetrics::incrementTemporarilyUnavailableErrors(long long n) {
    temporarilyUnavailableErrors.fetchAndAdd(n);
}

void OpDebug::AdditiveMetrics::incrementKeysInserted(long long n) {
    if (!keysInserted) {
        keysInserted = 0;
    }
    *keysInserted += n;
}

void OpDebug::AdditiveMetrics::incrementKeysDeleted(long long n) {
    if (!keysDeleted) {
        keysDeleted = 0;
    }
    *keysDeleted += n;
}

void OpDebug::AdditiveMetrics::incrementNreturned(long long n) {
    if (!nreturned) {
        nreturned = 0;
    }
    *nreturned += n;
}

void OpDebug::AdditiveMetrics::incrementNBatches() {
    if (!nBatches) {
        nBatches = 0;
    }
    ++(*nBatches);
}

void OpDebug::AdditiveMetrics::incrementNinserted(long long n) {
    if (!ninserted) {
        ninserted = 0;
    }
    *ninserted += n;
}

void OpDebug::AdditiveMetrics::incrementNUpserted(long long n) {
    if (!nUpserted) {
        nUpserted = 0;
    }
    *nUpserted += n;
}

void OpDebug::AdditiveMetrics::incrementExecutionTime(Microseconds n) {
    if (!executionTime) {
        executionTime = Microseconds{0};
    }
    *executionTime += n;
}

void OpDebug::AdditiveMetrics::incrementPrepareReadConflicts(long long n) {
    prepareReadConflicts.fetchAndAdd(n);
}

std::string OpDebug::AdditiveMetrics::report() const {
    StringBuilder s;

    OPDEBUG_TOSTRING_HELP_OPTIONAL("keysExamined", keysExamined);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("docsExamined", docsExamined);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("nMatched", nMatched);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("nreturned", nreturned);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("nBatches", nBatches);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("nModified", nModified);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("ninserted", ninserted);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("ndeleted", ndeleted);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("nUpserted", nUpserted);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("keysInserted", keysInserted);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("keysDeleted", keysDeleted);
    OPDEBUG_TOSTRING_HELP_ATOMIC("prepareReadConflicts", prepareReadConflicts);
    OPDEBUG_TOSTRING_HELP_ATOMIC("writeConflicts", writeConflicts);
    OPDEBUG_TOSTRING_HELP_ATOMIC("temporarilyUnavailableErrors", temporarilyUnavailableErrors);
    if (executionTime) {
        s << " durationMillis:" << durationCount<Milliseconds>(*executionTime);
    }

    return s.str();
}

void OpDebug::AdditiveMetrics::report(logv2::DynamicAttributes* pAttrs) const {
    OPDEBUG_TOATTR_HELP_OPTIONAL("keysExamined", keysExamined);
    OPDEBUG_TOATTR_HELP_OPTIONAL("docsExamined", docsExamined);
    OPDEBUG_TOATTR_HELP_OPTIONAL("nMatched", nMatched);
    OPDEBUG_TOATTR_HELP_OPTIONAL("nreturned", nreturned);
    OPDEBUG_TOATTR_HELP_OPTIONAL("nBatches", nBatches);
    OPDEBUG_TOATTR_HELP_OPTIONAL("nModified", nModified);
    OPDEBUG_TOATTR_HELP_OPTIONAL("ninserted", ninserted);
    OPDEBUG_TOATTR_HELP_OPTIONAL("ndeleted", ndeleted);
    OPDEBUG_TOATTR_HELP_OPTIONAL("nUpserted", nUpserted);
    OPDEBUG_TOATTR_HELP_OPTIONAL("keysInserted", keysInserted);
    OPDEBUG_TOATTR_HELP_OPTIONAL("keysDeleted", keysDeleted);
    OPDEBUG_TOATTR_HELP_ATOMIC("prepareReadConflicts", prepareReadConflicts);
    OPDEBUG_TOATTR_HELP_ATOMIC("writeConflicts", writeConflicts);
    OPDEBUG_TOATTR_HELP_ATOMIC("temporarilyUnavailableErrors", temporarilyUnavailableErrors);
    if (executionTime) {
        pAttrs->add("durationMillis", durationCount<Milliseconds>(*executionTime));
    }
}

BSONObj OpDebug::AdditiveMetrics::reportBSON() const {
    BSONObjBuilder b;
    OPDEBUG_APPEND_OPTIONAL(b, "keysExamined", keysExamined);
    OPDEBUG_APPEND_OPTIONAL(b, "docsExamined", docsExamined);
    OPDEBUG_APPEND_OPTIONAL(b, "nMatched", nMatched);
    OPDEBUG_APPEND_OPTIONAL(b, "nreturned", nreturned);
    OPDEBUG_APPEND_OPTIONAL(b, "nBatches", nBatches);
    OPDEBUG_APPEND_OPTIONAL(b, "nModified", nModified);
    OPDEBUG_APPEND_OPTIONAL(b, "ninserted", ninserted);
    OPDEBUG_APPEND_OPTIONAL(b, "ndeleted", ndeleted);
    OPDEBUG_APPEND_OPTIONAL(b, "nUpserted", nUpserted);
    OPDEBUG_APPEND_OPTIONAL(b, "keysInserted", keysInserted);
    OPDEBUG_APPEND_OPTIONAL(b, "keysDeleted", keysDeleted);
    OPDEBUG_APPEND_ATOMIC(b, "prepareReadConflicts", prepareReadConflicts);
    OPDEBUG_APPEND_ATOMIC(b, "writeConflicts", writeConflicts);
    OPDEBUG_APPEND_ATOMIC(b, "temporarilyUnavailableErrors", temporarilyUnavailableErrors);
    if (executionTime) {
        b.appendNumber("durationMillis", durationCount<Milliseconds>(*executionTime));
    }
    return b.obj();
}

}  // namespace mongo
