
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/curop.h"

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/global_lock_acquisition_tracker.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/json.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/metadata/client_metadata_ismaster.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/stringutils.h"

namespace mongo {

using std::string;

namespace {

// Lists the $-prefixed query options that can be passed alongside a wrapped query predicate for
// OP_QUERY find. The $orderby field is omitted because "orderby" (no dollar sign) is also allowed,
// and this requires special handling.
const std::vector<const char*> kDollarQueryModifiers = {
    "$hint",
    "$comment",
    "$maxScan",
    "$max",
    "$min",
    "$returnKey",
    "$showDiskLoc",
    "$snapshot",
    "$maxTimeMS",
};

}  // namespace

BSONObj upconvertQueryEntry(const BSONObj& query,
                            const NamespaceString& nss,
                            int ntoreturn,
                            int ntoskip) {
    BSONObjBuilder bob;

    bob.append("find", nss.coll());

    // Whether or not the query predicate is wrapped inside a "query" or "$query" field so that
    // other options can be passed alongside the predicate.
    bool predicateIsWrapped = false;

    // Extract the query predicate.
    BSONObj filter;
    if (query["query"].isABSONObj()) {
        predicateIsWrapped = true;
        bob.appendAs(query["query"], "filter");
    } else if (query["$query"].isABSONObj()) {
        predicateIsWrapped = true;
        bob.appendAs(query["$query"], "filter");
    } else if (!query.isEmpty()) {
        bob.append("filter", query);
    }

    if (ntoskip) {
        bob.append("skip", ntoskip);
    }
    if (ntoreturn) {
        bob.append("ntoreturn", ntoreturn);
    }

    // The remainder of the query options are only available if the predicate is passed in wrapped
    // form. If the predicate is not wrapped, we're done.
    if (!predicateIsWrapped) {
        return bob.obj();
    }

    // Extract the sort.
    if (auto elem = query["orderby"]) {
        bob.appendAs(elem, "sort");
    } else if (auto elem = query["$orderby"]) {
        bob.appendAs(elem, "sort");
    }

    // Add $-prefixed OP_QUERY modifiers, like $hint.
    for (auto modifier : kDollarQueryModifiers) {
        if (auto elem = query[modifier]) {
            // Use "+ 1" to omit the leading dollar sign.
            bob.appendAs(elem, modifier + 1);
        }
    }

    return bob.obj();
}

BSONObj upconvertGetMoreEntry(const NamespaceString& nss, CursorId cursorId, int ntoreturn) {
    return GetMoreRequest(nss,
                          cursorId,
                          ntoreturn,
                          boost::none,  // awaitDataTimeout
                          boost::none,  // term
                          boost::none   // lastKnownCommittedOpTime
                          )
        .toBSON();
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
    MONGO_DISALLOW_COPYING(CurOpStack);

public:
    CurOpStack() : _base(nullptr, this) {}

    /**
     * Returns the top of the CurOp stack.
     */
    CurOp* top() const {
        return _top;
    }

    /**
     * Adds "curOp" to the top of the CurOp stack for a client. Called by CurOp's constructor.
     */
    void push(OperationContext* opCtx, CurOp* curOp) {
        invariant(opCtx);
        if (_opCtx) {
            invariant(_opCtx == opCtx);
        } else {
            _opCtx = opCtx;
        }
        stdx::lock_guard<Client> lk(*_opCtx->getClient());
        push_nolock(curOp);
    }

    void push_nolock(CurOp* curOp) {
        invariant(!curOp->_parent);
        curOp->_parent = _top;
        _top = curOp;
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
            invariant(_opCtx);
            _opCtx->getClient()->lock();
        }
        invariant(_top);
        CurOp* retval = _top;
        _top = _top->_parent;
        if (shouldLock) {
            _opCtx->getClient()->unlock();
        }
        return retval;
    }

private:
    OperationContext* _opCtx = nullptr;

    // Top of the stack of CurOps for a Client.
    CurOp* _top = nullptr;

    // The bottom-most CurOp for a client.
    const CurOp _base;
};

const OperationContext::Decoration<CurOp::CurOpStack> CurOp::_curopStack =
    OperationContext::declareDecoration<CurOp::CurOpStack>();

CurOp* CurOp::get(const OperationContext* opCtx) {
    return get(*opCtx);
}

CurOp* CurOp::get(const OperationContext& opCtx) {
    return _curopStack(opCtx).top();
}

void CurOp::reportCurrentOpForClient(OperationContext* opCtx,
                                     Client* client,
                                     bool truncateOps,
                                     BSONObjBuilder* infoBuilder) {
    invariant(client);
    OperationContext* clientOpCtx = client->getOperationContext();

    const std::string hostName = getHostNameCachedAndPort();
    infoBuilder->append("host", hostName);

    client->reportState(*infoBuilder);
    const auto& clientMetadata = ClientMetadataIsMasterState::get(client).getClientMetadata();

    if (clientMetadata) {
        auto appName = clientMetadata.get().getApplicationName();
        if (!appName.empty()) {
            infoBuilder->append("appName", appName);
        }

        auto clientMetadataDocument = clientMetadata.get().getDocument();
        infoBuilder->append("clientMetadata", clientMetadataDocument);
    }

    // Fill out the rest of the BSONObj with opCtx specific details.
    infoBuilder->appendBool("active", static_cast<bool>(clientOpCtx));
    infoBuilder->append("currentOpTime",
                        opCtx->getServiceContext()->getPreciseClockSource()->now().toString());

    if (clientOpCtx) {
        infoBuilder->append("opid", clientOpCtx->getOpID());
        if (clientOpCtx->isKillPending()) {
            infoBuilder->append("killPending", true);
        }

        if (auto lsid = clientOpCtx->getLogicalSessionId()) {
            BSONObjBuilder lsidBuilder(infoBuilder->subobjStart("lsid"));
            lsid->serialize(&lsidBuilder);
        }

        CurOp::get(clientOpCtx)->reportState(infoBuilder, truncateOps);
    }
}

CurOp::CurOp(OperationContext* opCtx) : CurOp(opCtx, &_curopStack(opCtx)) {
    // If this is a sub-operation, we store the snapshot of lock stats as the base lock stats of the
    // current operation.
    if (_parent != nullptr)
        _lockStatsBase = opCtx->lockState()->getLockerInfo(boost::none)->stats;
}

CurOp::CurOp(OperationContext* opCtx, CurOpStack* stack) : _stack(stack) {
    if (opCtx) {
        _stack->push(opCtx, this);
    } else {
        _stack->push_nolock(this);
    }
}

CurOp::~CurOp() {
    invariant(this == _stack->pop());
}

void CurOp::setGenericOpRequestDetails(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const Command* command,
                                       BSONObj cmdObj,
                                       NetworkOp op) {
    // Set the _isCommand flags based on network op only. For legacy writes on mongoS, we resolve
    // them to OpMsgRequests and then pass them into the Commands path, so having a valid Command*
    // here does not guarantee that the op was issued from the client using a command protocol.
    const bool isCommand = (op == dbMsg || op == dbCommand || (op == dbQuery && nss.isCommand()));
    auto logicalOp = (command ? command->getLogicalOp() : networkOpToLogicalOp(op));

    stdx::lock_guard<Client> clientLock(*opCtx->getClient());
    _isCommand = _debug.iscommand = isCommand;
    _logicalOp = _debug.logicalOp = logicalOp;
    _networkOp = _debug.networkOp = op;
    _opDescription = cmdObj;
    _command = command;
    _ns = nss.ns();
}

ProgressMeter& CurOp::setMessage_inlock(const char* msg,
                                        std::string name,
                                        unsigned long long progressMeterTotal,
                                        int secondsBetween) {
    if (progressMeterTotal) {
        if (_progressMeter.isActive()) {
            error() << "old _message: " << redact(_message) << " new message:" << redact(msg);
            verify(!_progressMeter.isActive());
        }
        _progressMeter.reset(progressMeterTotal, secondsBetween);
        _progressMeter.setName(name);
    } else {
        _progressMeter.finished();
    }
    _message = msg;
    return _progressMeter;
}

void CurOp::setNS_inlock(StringData ns) {
    _ns = ns.toString();
}

void CurOp::ensureStarted() {
    if (_start == 0) {
        _start = curTimeMicros64();
    }
}

void CurOp::enter_inlock(const char* ns, boost::optional<int> dbProfileLevel) {
    ensureStarted();
    _ns = ns;
    if (dbProfileLevel) {
        raiseDbProfileLevel(*dbProfileLevel);
    }
}

void CurOp::raiseDbProfileLevel(int dbProfileLevel) {
    _dbprofile = std::max(dbProfileLevel, _dbprofile);
}

bool CurOp::completeAndLogOperation(OperationContext* opCtx,
                                    logger::LogComponent component,
                                    boost::optional<size_t> responseLength,
                                    boost::optional<long long> slowMsOverride,
                                    bool forceLog) {
    // Log the operation if it is eligible according to the current slowMS and sampleRate settings.
    const bool shouldLogOp = (forceLog || shouldLog(component, logger::LogSeverity::Debug(1)));
    const long long slowMs = slowMsOverride.value_or(serverGlobalParams.slowMS);

    const auto client = opCtx->getClient();

    // Record the size of the response returned to the client, if applicable.
    if (responseLength) {
        _debug.responseLength = *responseLength;
    }

    // Obtain the total execution time of this operation.
    _end = curTimeMicros64();
    _debug.executionTimeMicros = durationCount<Microseconds>(elapsedTimeExcludingPauses());

    const bool shouldSample =
        client->getPrng().nextCanonicalDouble() < serverGlobalParams.sampleRate;

    if (shouldLogOp || (shouldSample && _debug.executionTimeMicros > slowMs * 1000LL)) {
        auto lockerInfo = opCtx->lockState()->getLockerInfo(_lockStatsBase);
        const GlobalLockAcquisitionTracker& globalLockTracker =
            GlobalLockAcquisitionTracker::get(opCtx);
        if (_debug.storageStats == nullptr && globalLockTracker.getGlobalLockTaken() &&
            opCtx->getServiceContext()->getStorageEngine()) {
            // Do not fetch operation statistics again if we have already got them (for instance,
            // as a part of stashing the transaction).
            // Take a lock before calling into the storage engine to prevent racing against a
            // shutdown. Any operation that used a storage engine would have at-least held a
            // global lock at one point, hence we limit our lock acquisition to such operations.
            // We can get here and our lock acquisition be timed out or interrupted, log a
            // message if that happens.
            try {
                Lock::GlobalLock lk(opCtx,
                                    MODE_IS,
                                    Date_t::now() + Milliseconds(500),
                                    Lock::InterruptBehavior::kLeaveUnlocked);
                if (lk.isLocked()) {
                    _debug.storageStats = opCtx->recoveryUnit()->getOperationStatistics();
                } else {
                    warning(component) << "Unable to gather storage statistics for a slow "
                                          "operation due to lock aquire timeout";
                }
            } catch (const ExceptionForCat<ErrorCategory::Interruption>&) {
                warning(component) << "Unable to gather storage statistics for a slow "
                                      "operation due to interrupt";
            }
        }
        log(component) << _debug.report(client, *this, (lockerInfo ? &lockerInfo->stats : nullptr));
    }

    // Return 'true' if this operation should also be added to the profiler.
    return shouldDBProfile(shouldSample);
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
        // Generate an abbreviated serialization for the object, by passing false as the
        // "full" argument to obj.toString().
        std::string objToString = obj.toString();
        if (objToString.size() > *maxSize) {
            // objToString is still too long, so we append to the builder a truncated form
            // of objToString concatenated with "...".  Instead of creating a new string
            // temporary, mutate objToString to do this (we know that we can mutate
            // characters in objToString up to and including objToString[maxSize]).
            objToString[*maxSize - 3] = '.';
            objToString[*maxSize - 2] = '.';
            objToString[*maxSize - 1] = '.';
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

void CurOp::reportState(BSONObjBuilder* builder, bool truncateOps) {
    if (_start) {
        builder->append("secs_running", durationCount<Seconds>(elapsedTimeTotal()));
        builder->append("microsecs_running", durationCount<Microseconds>(elapsedTimeTotal()));
    }

    builder->append("op", logicalOpToString(_logicalOp));
    builder->append("ns", _ns);

    // When the currentOp command is run, it returns a single response object containing all current
    // operations; this request will fail if the response exceeds the 16MB document limit. By
    // contrast, the $currentOp aggregation stage does not have this restriction. If 'truncateOps'
    // is true, limit the size of each op to 1000 bytes. Otherwise, do not truncate.
    const boost::optional<size_t> maxQuerySize{truncateOps, 1000};

    appendAsObjOrString("command", _opDescription, maxQuerySize, builder);

    if (!_originatingCommand.isEmpty()) {
        appendAsObjOrString("originatingCommand", _originatingCommand, maxQuerySize, builder);
    }

    if (!_planSummary.empty()) {
        builder->append("planSummary", _planSummary);
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

    builder->append("numYields", _numYields);
}

namespace {
StringData getProtoString(int op) {
    if (op == dbMsg) {
        return "op_msg";
    } else if (op == dbQuery) {
        return "op_query";
    } else if (op == dbCommand) {
        return "op_command";
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
#define OPDEBUG_TOSTRING_HELP_OPTIONAL(x, y) \
    if (y)                                   \
    s << " " x ":" << (*y)

string OpDebug::report(Client* client,
                       const CurOp& curop,
                       const SingleThreadedLockStats* lockStats) const {
    StringBuilder s;
    if (iscommand)
        s << "command ";
    else
        s << networkOpToString(networkOp) << ' ';

    s << curop.getNS();

    const auto& clientMetadata = ClientMetadataIsMasterState::get(client).getClientMetadata();
    if (clientMetadata) {
        auto appName = clientMetadata.get().getApplicationName();
        if (!appName.empty()) {
            s << " appName: \"" << escape(appName) << '\"';
        }
    }

    auto query = curop.opDescription();
    if (!query.isEmpty()) {
        s << " command: ";
        if (iscommand) {
            const Command* curCommand = curop.getCommand();
            if (curCommand) {
                mutablebson::Document cmdToLog(query, mutablebson::Document::kInPlaceDisabled);
                curCommand->redactForLogging(&cmdToLog);
                s << curCommand->getName() << " ";
                s << redact(cmdToLog.getObject());
            } else {
                // Should not happen but we need to handle curCommand == NULL gracefully.
                // We don't know what the request payload is intended to be, so it might be
                // sensitive, and we don't know how to redact it properly without a 'Command*'.
                // So we just don't log it at all.
                s << "unrecognized";
            }
        } else {
            s << redact(query);
        }
    }

    auto originatingCommand = curop.originatingCommand();
    if (!originatingCommand.isEmpty()) {
        s << " originatingCommand: " << redact(originatingCommand);
    }

    if (!curop.getPlanSummary().empty()) {
        s << " planSummary: " << curop.getPlanSummary().toString();
    }

    OPDEBUG_TOSTRING_HELP(nShards);
    OPDEBUG_TOSTRING_HELP(cursorid);
    OPDEBUG_TOSTRING_HELP(ntoreturn);
    OPDEBUG_TOSTRING_HELP(ntoskip);
    OPDEBUG_TOSTRING_HELP_BOOL(exhaust);

    OPDEBUG_TOSTRING_HELP_OPTIONAL("keysExamined", additiveMetrics.keysExamined);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("docsExamined", additiveMetrics.docsExamined);
    OPDEBUG_TOSTRING_HELP_BOOL(hasSortStage);
    OPDEBUG_TOSTRING_HELP_BOOL(fromMultiPlanner);
    OPDEBUG_TOSTRING_HELP_BOOL(replanned);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("nMatched", additiveMetrics.nMatched);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("nModified", additiveMetrics.nModified);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("ninserted", additiveMetrics.ninserted);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("ndeleted", additiveMetrics.ndeleted);
    OPDEBUG_TOSTRING_HELP_BOOL(fastmodinsert);
    OPDEBUG_TOSTRING_HELP_BOOL(upsert);
    OPDEBUG_TOSTRING_HELP_BOOL(cursorExhausted);

    OPDEBUG_TOSTRING_HELP_OPTIONAL("nmoved", additiveMetrics.nmoved);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("keysInserted", additiveMetrics.keysInserted);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("keysDeleted", additiveMetrics.keysDeleted);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("prepareReadConflicts", additiveMetrics.prepareReadConflicts);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("writeConflicts", additiveMetrics.writeConflicts);

    s << " numYields:" << curop.numYields();
    OPDEBUG_TOSTRING_HELP(nreturned);

    if (!errInfo.isOK()) {
        s << " ok:" << 0;
        if (!errInfo.reason().empty()) {
            s << " errMsg:\"" << escape(redact(errInfo.reason())) << "\"";
        }
        s << " errName:" << errInfo.code();
        s << " errCode:" << static_cast<int>(errInfo.code());
    }

    if (responseLength > 0) {
        s << " reslen:" << responseLength;
    }

    if (lockStats) {
        BSONObjBuilder locks;
        lockStats->report(&locks);
        s << " locks:" << locks.obj().toString();
    }

    if (storageStats) {
        s << " storage:" << storageStats->toBSON().toString();
    }

    if (iscommand) {
        s << " protocol:" << getProtoString(networkOp);
    }

    s << " " << (executionTimeMicros / 1000) << "ms";

    return s.str();
}

#define OPDEBUG_APPEND_NUMBER(x) \
    if (x != -1)                 \
    b.appendNumber(#x, (x))
#define OPDEBUG_APPEND_BOOL(x) \
    if (x)                     \
    b.appendBool(#x, (x))
#define OPDEBUG_APPEND_OPTIONAL(x, y) \
    if (y)                            \
    b.appendNumber(x, (*y))

void OpDebug::append(const CurOp& curop,
                     const SingleThreadedLockStats& lockStats,
                     BSONObjBuilder& b) const {
    const size_t maxElementSize = 50 * 1024;

    b.append("op", logicalOpToString(logicalOp));

    NamespaceString nss = NamespaceString(curop.getNS());
    b.append("ns", nss.ns());

    appendAsObjOrString("command", curop.opDescription(), maxElementSize, &b);

    auto originatingCommand = curop.originatingCommand();
    if (!originatingCommand.isEmpty()) {
        appendAsObjOrString("originatingCommand", originatingCommand, maxElementSize, &b);
    }

    OPDEBUG_APPEND_NUMBER(nShards);
    OPDEBUG_APPEND_NUMBER(cursorid);
    OPDEBUG_APPEND_BOOL(exhaust);

    OPDEBUG_APPEND_OPTIONAL("keysExamined", additiveMetrics.keysExamined);
    OPDEBUG_APPEND_OPTIONAL("docsExamined", additiveMetrics.docsExamined);
    OPDEBUG_APPEND_BOOL(hasSortStage);
    OPDEBUG_APPEND_BOOL(fromMultiPlanner);
    OPDEBUG_APPEND_BOOL(replanned);
    OPDEBUG_APPEND_OPTIONAL("nMatched", additiveMetrics.nMatched);
    OPDEBUG_APPEND_OPTIONAL("nModified", additiveMetrics.nModified);
    OPDEBUG_APPEND_OPTIONAL("ninserted", additiveMetrics.ninserted);
    OPDEBUG_APPEND_OPTIONAL("ndeleted", additiveMetrics.ndeleted);
    OPDEBUG_APPEND_BOOL(fastmodinsert);
    OPDEBUG_APPEND_BOOL(upsert);
    OPDEBUG_APPEND_BOOL(cursorExhausted);

    OPDEBUG_APPEND_OPTIONAL("nmoved", additiveMetrics.nmoved);
    OPDEBUG_APPEND_OPTIONAL("keysInserted", additiveMetrics.keysInserted);
    OPDEBUG_APPEND_OPTIONAL("keysDeleted", additiveMetrics.keysDeleted);
    OPDEBUG_APPEND_OPTIONAL("prepareReadConflicts", additiveMetrics.prepareReadConflicts);
    OPDEBUG_APPEND_OPTIONAL("writeConflicts", additiveMetrics.writeConflicts);

    b.appendNumber("numYield", curop.numYields());
    OPDEBUG_APPEND_NUMBER(nreturned);

    {
        BSONObjBuilder locks(b.subobjStart("locks"));
        lockStats.report(&locks);
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

    OPDEBUG_APPEND_NUMBER(responseLength);
    if (iscommand) {
        b.append("protocol", getProtoString(networkOp));
    }
    b.appendIntOrLL("millis", executionTimeMicros / 1000);

    if (!curop.getPlanSummary().empty()) {
        b.append("planSummary", curop.getPlanSummary());
    }

    if (!execStats.isEmpty()) {
        b.append("execStats", execStats);
    }
}

void OpDebug::setPlanSummaryMetrics(const PlanSummaryStats& planSummaryStats) {
    additiveMetrics.keysExamined = planSummaryStats.totalKeysExamined;
    additiveMetrics.docsExamined = planSummaryStats.totalDocsExamined;
    hasSortStage = planSummaryStats.hasSortStage;
    fromMultiPlanner = planSummaryStats.fromMultiPlanner;
    replanned = planSummaryStats.replanned;
}

namespace {

/**
 * Adds two boost::optional long longs together. Returns boost::none if both 'lhs' and 'rhs' are
 * uninitialized, or the sum of 'lhs' and 'rhs' if they are both initialized. Returns 'lhs' if only
 * 'rhs' is uninitialized and vice-versa.
 */
boost::optional<long long> addOptionalLongs(const boost::optional<long long>& lhs,
                                            const boost::optional<long long>& rhs) {
    if (!rhs) {
        return lhs;
    }
    return lhs ? (*lhs + *rhs) : rhs;
}
}  // namespace

void OpDebug::AdditiveMetrics::add(const AdditiveMetrics& otherMetrics) {
    keysExamined = addOptionalLongs(keysExamined, otherMetrics.keysExamined);
    docsExamined = addOptionalLongs(docsExamined, otherMetrics.docsExamined);
    nMatched = addOptionalLongs(nMatched, otherMetrics.nMatched);
    nModified = addOptionalLongs(nModified, otherMetrics.nModified);
    ninserted = addOptionalLongs(ninserted, otherMetrics.ninserted);
    ndeleted = addOptionalLongs(ndeleted, otherMetrics.ndeleted);
    nmoved = addOptionalLongs(nmoved, otherMetrics.nmoved);
    keysInserted = addOptionalLongs(keysInserted, otherMetrics.keysInserted);
    keysDeleted = addOptionalLongs(keysDeleted, otherMetrics.keysDeleted);
    prepareReadConflicts =
        addOptionalLongs(prepareReadConflicts, otherMetrics.prepareReadConflicts);
    writeConflicts = addOptionalLongs(writeConflicts, otherMetrics.writeConflicts);
}

bool OpDebug::AdditiveMetrics::equals(const AdditiveMetrics& otherMetrics) {
    return keysExamined == otherMetrics.keysExamined && docsExamined == otherMetrics.docsExamined &&
        nMatched == otherMetrics.nMatched && nModified == otherMetrics.nModified &&
        ninserted == otherMetrics.ninserted && ndeleted == otherMetrics.ndeleted &&
        nmoved == otherMetrics.nmoved && keysInserted == otherMetrics.keysInserted &&
        keysDeleted == otherMetrics.keysDeleted &&
        prepareReadConflicts == otherMetrics.prepareReadConflicts &&
        writeConflicts == otherMetrics.writeConflicts;
}

void OpDebug::AdditiveMetrics::incrementWriteConflicts(long long n) {
    if (!writeConflicts) {
        writeConflicts = 0;
    }
    *writeConflicts += n;
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

void OpDebug::AdditiveMetrics::incrementNmoved(long long n) {
    if (!nmoved) {
        nmoved = 0;
    }
    *nmoved += n;
}

void OpDebug::AdditiveMetrics::incrementNinserted(long long n) {
    if (!ninserted) {
        ninserted = 0;
    }
    *ninserted += n;
}

void OpDebug::AdditiveMetrics::incrementPrepareReadConflicts(long long n) {
    if (!prepareReadConflicts) {
        prepareReadConflicts = 0;
    }
    *prepareReadConflicts += n;
}

string OpDebug::AdditiveMetrics::report() {
    StringBuilder s;

    OPDEBUG_TOSTRING_HELP_OPTIONAL("keysExamined", keysExamined);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("docsExamined", docsExamined);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("nMatched", nMatched);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("nModified", nModified);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("ninserted", ninserted);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("ndeleted", ndeleted);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("nmoved", nmoved);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("keysInserted", keysInserted);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("keysDeleted", keysDeleted);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("prepareReadConflicts", prepareReadConflicts);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("writeConflicts", writeConflicts);

    return s.str();
}

}  // namespace mongo
