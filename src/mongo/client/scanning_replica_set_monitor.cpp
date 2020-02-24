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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/scanning_replica_set_monitor.h"

#include <algorithm>
#include <limits>
#include <random>

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/client/connpool.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/scanning_replica_set_monitor_internal.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/background.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"
#include "mongo/util/string_map.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::numeric_limits;
using std::set;
using std::shared_ptr;
using std::string;
using std::vector;

namespace {

// Pull nested types to top-level scope
typedef ScanningReplicaSetMonitor::IsMasterReply IsMasterReply;
typedef ScanningReplicaSetMonitor::ScanState ScanState;
typedef ScanningReplicaSetMonitor::ScanStatePtr ScanStatePtr;
typedef ScanningReplicaSetMonitor::SetState SetState;
typedef ScanningReplicaSetMonitor::SetStatePtr SetStatePtr;
typedef ScanningReplicaSetMonitor::Refresher Refresher;
typedef ScanState::UnconfirmedReplies UnconfirmedReplies;
typedef SetState::Node Node;
typedef SetState::Nodes Nodes;
using executor::TaskExecutor;
using CallbackArgs = TaskExecutor::CallbackArgs;
using CallbackHandle = TaskExecutor::CallbackHandle;

// Intentionally chosen to compare worse than all known latencies.
const int64_t unknownLatency = numeric_limits<int64_t>::max();

const ReadPreferenceSetting kPrimaryOnlyReadPreference(ReadPreference::PrimaryOnly, TagSet());

//
// Helpers for stl algorithms
//

bool isMaster(const Node& node) {
    return node.isMaster;
}

bool opTimeGreater(const Node* lhs, const Node* rhs) {
    return lhs->opTime > rhs->opTime;
}

bool compareLatencies(const Node* lhs, const Node* rhs) {
    // NOTE: this automatically compares Node::unknownLatency worse than all others.
    return lhs->latencyMicros < rhs->latencyMicros;
}

bool hostsEqual(const Node& lhs, const HostAndPort& rhs) {
    return lhs.host == rhs;
}

// Allows comparing two Nodes, or a HostAndPort and a Node.
// NOTE: the two HostAndPort overload is only needed to support extra checks in some STL
// implementations. For simplicity, no comparator should be used with collections of just
// HostAndPort.
struct CompareHosts {
    bool operator()(const Node& lhs, const Node& rhs) {
        return lhs.host < rhs.host;
    }
    bool operator()(const Node& lhs, const HostAndPort& rhs) {
        return lhs.host < rhs;
    }
    bool operator()(const HostAndPort& lhs, const Node& rhs) {
        return lhs < rhs.host;
    }
    bool operator()(const HostAndPort& lhs, const HostAndPort& rhs) {
        return lhs < rhs;
    }
} compareHosts;  // like an overloaded function, but able to pass to stl algorithms

// The following structs should be treated as functions returning a UnaryPredicate.
// Usage example: std::find_if(nodes.begin(), nodes.end(), HostIs(someHost));
// They all hold their constructor argument by reference.

struct HostIs {
    explicit HostIs(const HostAndPort& host) : _host(host) {}
    bool operator()(const HostAndPort& host) {
        return host == _host;
    }
    bool operator()(const Node& node) {
        return node.host == _host;
    }
    const HostAndPort& _host;
};

struct HostNotIn {
    explicit HostNotIn(const std::set<HostAndPort>& hosts) : _hosts(hosts) {}
    bool operator()(const HostAndPort& host) {
        return !_hosts.count(host);
    }
    bool operator()(const Node& node) {
        return !_hosts.count(node.host);
    }
    const std::set<HostAndPort>& _hosts;
};

int32_t pingTimeMillis(const Node& node) {
    auto latencyMillis = node.latencyMicros / 1000;
    if (latencyMillis > numeric_limits<int32_t>::max()) {
        // In particular, Node::unknownLatency does not fit in an int32.
        return numeric_limits<int32_t>::max();
    }
    return latencyMillis;
}

/**
 * Replica set refresh period on the task executor.
 */
const Seconds kDefaultRefreshPeriod(30);
}  // namespace

ScanningReplicaSetMonitor::ScanningReplicaSetMonitor(const SetStatePtr& initialState)
    : _state(initialState) {}

ScanningReplicaSetMonitor::ScanningReplicaSetMonitor(const MongoURI& uri)
    : ScanningReplicaSetMonitor(
          std::make_shared<SetState>(uri,
                                     &ReplicaSetMonitorManager::get()->getNotifier(),
                                     ReplicaSetMonitorManager::get()->getExecutor())) {}

void ScanningReplicaSetMonitor::init() {
    if (areRefreshRetriesDisabledForTest()) {
        // This is for MockReplicaSet. Those tests want to control when scanning happens.
        LOGV2_WARNING(24088,
                      "*** Not starting background refresh because refresh retries are disabled.");
        return;
    }

    {
        stdx::lock_guard lk(_state->mutex);
        _state->init();
    }
}

void ScanningReplicaSetMonitor::drop() {
    {
        stdx::lock_guard lk(_state->mutex);
        _state->drop();
    }
}

ScanningReplicaSetMonitor::~ScanningReplicaSetMonitor() {
    drop();
}

template <typename Callback>
auto ScanningReplicaSetMonitor::SetState::scheduleWorkAt(Date_t when, Callback&& cb) const {
    auto wrappedCallback = [cb = std::forward<Callback>(cb),
                            anchor = shared_from_this()](const CallbackArgs& cbArgs) mutable {
        if (ErrorCodes::isCancelationError(cbArgs.status)) {
            // Do no more work if we're removed or canceled
            return;
        }
        invariant(cbArgs.status);

        stdx::lock_guard lk(anchor->mutex);
        if (anchor->isDropped) {
            return;
        }

        cb(cbArgs);
    };
    return executor->scheduleWorkAt(std::move(when), std::move(wrappedCallback));
}

void ScanningReplicaSetMonitor::SetState::rescheduleRefresh(SchedulingStrategy strategy) {
    // Reschedule the refresh

    if (!executor || isMocked) {
        // Without an executor, we can't do refreshes -- we're in a test
        return;
    }

    if (isDropped) {  // already removed so no need to refresh
        LOGV2_DEBUG(24070,
                    1,
                    "Stopping refresh for replica set {name} because it's removed",
                    "name"_attr = name);
        return;
    }

    Milliseconds period = refreshPeriod;
    if (isExpedited) {
        period = std::min<Milliseconds>(period, kExpeditedRefreshPeriod);
    }

    auto currentTime = now();
    auto possibleNextScanTime = currentTime + period;
    if (refresherHandle &&                                   //
        (strategy == SchedulingStrategy::kKeepEarlyScan) &&  //
        (nextScanTime > currentTime) &&                      //
        (possibleNextScanTime >= nextScanTime)) {
        // If the next scan would be sooner than our desired, why cancel?
        return;
    }

    // Cancel out the last refresh
    if (auto currentHandle = std::exchange(refresherHandle, {})) {
        executor->cancel(currentHandle);
    }

    nextScanTime = possibleNextScanTime;
    LOGV2_DEBUG(24071,
                1,
                "Next replica set scan scheduled for {nextScanTime}",
                "nextScanTime"_attr = nextScanTime);
    auto swHandle = scheduleWorkAt(nextScanTime, [this](const CallbackArgs& cbArgs) {
        if (cbArgs.myHandle != refresherHandle)
            return;  // We've been replaced!

        // It is possible that a waiter will have already expired by the point of this rescan.
        // Thus we notify here to trigger that logic.
        notify();

        _ensureScanInProgress(shared_from_this());

        // And now we set up the next one
        rescheduleRefresh(SchedulingStrategy::kKeepEarlyScan);
    });

    if (ErrorCodes::isShutdownError(swHandle.getStatus().code())) {
        LOGV2_DEBUG(24072,
                    1,
                    "Cant schedule refresh for {name}. Executor shutdown in progress",
                    "name"_attr = name);
        return;
    }

    if (!swHandle.isOK()) {
        LOGV2_FATAL(24092,
                    "Can't continue refresh for replica set {name} due to {swHandle_getStatus}",
                    "name"_attr = name,
                    "swHandle_getStatus"_attr = redact(swHandle.getStatus()));
        fassertFailed(40140);
    }

    refresherHandle = std::move(swHandle.getValue());
}

SemiFuture<HostAndPort> ScanningReplicaSetMonitor::getHostOrRefresh(
    const ReadPreferenceSetting& criteria, Milliseconds maxWait) {
    return _getHostsOrRefresh(criteria, maxWait)
        .then([](const auto& hosts) {
            invariant(hosts.size());
            return hosts[0];
        })
        .semi();
}

SemiFuture<std::vector<HostAndPort>> ScanningReplicaSetMonitor::getHostsOrRefresh(
    const ReadPreferenceSetting& criteria, Milliseconds maxWait) {
    return _getHostsOrRefresh(criteria, maxWait).semi();
}

Future<std::vector<HostAndPort>> ScanningReplicaSetMonitor::_getHostsOrRefresh(
    const ReadPreferenceSetting& criteria, Milliseconds maxWait) {

    stdx::lock_guard<Latch> lk(_state->mutex);
    if (_state->isDropped) {
        return Status(ErrorCodes::ReplicaSetMonitorRemoved,
                      str::stream()
                          << "ScanningReplicaSetMonitor for set " << getName() << " is removed");
    }

    auto out = _state->getMatchingHosts(criteria);
    if (!out.empty())
        return {std::move(out)};

    // Fail early without doing any more work if the timeout has already expired.
    if (maxWait <= Milliseconds(0))
        return _state->makeUnsatisfedReadPrefError(criteria);

    // TODO look into putting all PrimaryOnly waiters on a single SharedPromise. The tricky part is
    // dealing with maxWait.
    auto pf = makePromiseFuture<decltype(out)>();
    _state->waiters.emplace_back(
        SetState::Waiter{_state->now() + maxWait, criteria, std::move(pf.promise)});

    // This must go after we set up the wait state to correctly handle unittests using
    // MockReplicaSet.
    _ensureScanInProgress(_state);

    // Switch to expedited scanning.
    _state->isExpedited = true;
    _state->rescheduleRefresh(SetState::SchedulingStrategy::kKeepEarlyScan);

    return std::move(pf.future);
}
HostAndPort ScanningReplicaSetMonitor::getMasterOrUassert() {
    return getHostOrRefresh(kPrimaryOnlyReadPreference).get();
}

void ScanningReplicaSetMonitor::failedHost(const HostAndPort& host, const Status& status) {
    stdx::lock_guard<Latch> lk(_state->mutex);
    Node* node = _state->findNode(host);
    if (node)
        node->markFailed(status);
    if (kDebugBuild)
        _state->checkInvariants();
}

bool ScanningReplicaSetMonitor::isPrimary(const HostAndPort& host) const {
    stdx::lock_guard<Latch> lk(_state->mutex);
    Node* node = _state->findNode(host);
    return node ? node->isMaster : false;
}

bool ScanningReplicaSetMonitor::isHostUp(const HostAndPort& host) const {
    stdx::lock_guard<Latch> lk(_state->mutex);
    Node* node = _state->findNode(host);
    return node ? node->isUp : false;
}

int ScanningReplicaSetMonitor::getMinWireVersion() const {
    stdx::lock_guard<Latch> lk(_state->mutex);
    int minVersion = 0;
    for (const auto& host : _state->nodes) {
        if (host.isUp) {
            minVersion = std::max(minVersion, host.minWireVersion);
        }
    }

    return minVersion;
}

int ScanningReplicaSetMonitor::getMaxWireVersion() const {
    stdx::lock_guard<Latch> lk(_state->mutex);
    int maxVersion = std::numeric_limits<int>::max();
    for (const auto& host : _state->nodes) {
        if (host.isUp) {
            maxVersion = std::min(maxVersion, host.maxWireVersion);
        }
    }

    return maxVersion;
}

std::string ScanningReplicaSetMonitor::getName() const {
    // name is const so don't need to lock
    return _state->name;
}

std::string ScanningReplicaSetMonitor::getServerAddress() const {
    stdx::lock_guard<Latch> lk(_state->mutex);
    // We return our setUri until first confirmation
    return _state->seedConnStr.isValid() ? _state->seedConnStr.toString()
                                         : _state->setUri.connectionString().toString();
}

const MongoURI& ScanningReplicaSetMonitor::getOriginalUri() const {
    // setUri is const so no need to lock.
    return _state->setUri;
}

bool ScanningReplicaSetMonitor::contains(const HostAndPort& host) const {
    stdx::lock_guard<Latch> lk(_state->mutex);
    return _state->seedNodes.count(host);
}

// TODO move to correct order with non-statics before pushing
void ScanningReplicaSetMonitor::appendInfo(BSONObjBuilder& bsonObjBuilder, bool forFTDC) const {
    stdx::lock_guard<Latch> lk(_state->mutex);

    BSONObjBuilder monitorInfo(bsonObjBuilder.subobjStart(getName()));
    if (forFTDC) {
        for (size_t i = 0; i < _state->nodes.size(); i++) {
            const Node& node = _state->nodes[i];
            monitorInfo.appendNumber(node.host.toString(), pingTimeMillis(node));
        }
        return;
    }

    // NOTE: the format here must be consistent for backwards compatibility
    BSONArrayBuilder hosts(monitorInfo.subarrayStart("hosts"));
    for (size_t i = 0; i < _state->nodes.size(); i++) {
        const Node& node = _state->nodes[i];

        BSONObjBuilder builder;
        builder.append("addr", node.host.toString());
        builder.append("ok", node.isUp);
        builder.append("ismaster", node.isMaster);  // intentionally not camelCase
        builder.append("hidden", false);            // we don't keep hidden nodes in the set
        builder.append("secondary", node.isUp && !node.isMaster);
        builder.append("pingTimeMillis", pingTimeMillis(node));

        if (!node.tags.isEmpty()) {
            builder.append("tags", node.tags);
        }

        hosts.append(builder.obj());
    }
}
bool ScanningReplicaSetMonitor::isKnownToHaveGoodPrimary() const {
    stdx::lock_guard<Latch> lk(_state->mutex);

    for (const auto& node : _state->nodes) {
        if (node.isMaster) {
            return true;
        }
    }

    return false;
}

Seconds ScanningReplicaSetMonitor::getDefaultRefreshPeriod() {
    Seconds r = kDefaultRefreshPeriod;
    static constexpr auto kPeriodField = "period"_sd;
    globalFailPointRegistry()
        .find("modifyReplicaSetMonitorDefaultRefreshPeriod")
        ->executeIf([&r](const BSONObj& data) { r = Seconds{data.getIntField(kPeriodField)}; },
                    [](const BSONObj& data) { return data.hasField(kPeriodField); });
    return r;
}

void ScanningReplicaSetMonitor::runScanForMockReplicaSet() {
    stdx::lock_guard<Latch> lk(_state->mutex);
    _ensureScanInProgress(_state);

    // This function should only be called from tests using MockReplicaSet and they should use the
    // synchronous path to complete before returning.
    invariant(_state->currentScan == nullptr);
}

void ScanningReplicaSetMonitor::_ensureScanInProgress(const SetStatePtr& state) {
    Refresher(state).scheduleNetworkRequests();
}

Refresher::Refresher(const SetStatePtr& setState) : _set(setState), _scan(setState->currentScan) {
    if (_scan) {
        _set->rescheduleRefresh(SetState::SchedulingStrategy::kKeepEarlyScan);
        _scan->retryAllTriedHosts(_set->rand);
        return;  // participate in in-progress scan
    }

    startNewScan();
}

void Refresher::scheduleNetworkRequests() {
    for (auto ns = getNextStep(); ns.step == NextStep::CONTACT_HOST; ns = getNextStep()) {
        if (!_set->executor || _set->isMocked) {
            // If we're mocked, just schedule an isMaster
            scheduleIsMaster(ns.host);
            continue;
        }

        // cancel any scheduled isMaster calls that haven't yet been called
        Node* node = _set->findOrCreateNode(ns.host);
        if (auto handle = std::exchange(node->scheduledIsMasterHandle, {})) {
            _set->executor->cancel(handle);
        }

        // ensure that the call to isMaster is scheduled at most every 500ms
        auto swHandle =
            _set->scheduleWorkAt(node->nextPossibleIsMasterCall,
                                 [*this, host = ns.host](const CallbackArgs& cbArgs) mutable {
                                     scheduleIsMaster(host);
                                 });

        if (ErrorCodes::isShutdownError(swHandle.getStatus().code())) {
            _scan->markHostsToScanAsTried();
            break;
        }

        if (!swHandle.isOK()) {
            LOGV2_FATAL(
                24093,
                "Can't continue scan for replica set {set_name} due to {swHandle_getStatus}",
                "set_name"_attr = _set->name,
                "swHandle_getStatus"_attr = redact(swHandle.getStatus()));
            fassertFailed(31176);
        }

        node->scheduledIsMasterHandle = uassertStatusOK(std::move(swHandle));
    }

    if (kDebugBuild)
        _set->checkInvariants();
}

void Refresher::scheduleIsMaster(const HostAndPort& host) {
    if (_set->isMocked) {
        // MockReplicaSet only works with DBClient-style access since it injects itself into the
        // ScopedDbConnection pool connection creation.
        try {
            ScopedDbConnection conn(ConnectionString(host), kCheckTimeout.count());

            auto timer = Timer();
            auto reply = BSONObj();
            bool ignoredOutParam = false;
            conn->isMaster(ignoredOutParam, &reply);
            conn.done();  // return to pool on success.

            receivedIsMaster(host, timer.micros(), reply);
        } catch (DBException& ex) {
            failedHost(host, ex.toStatus());
        }

        return;
    }

    auto request = executor::RemoteCommandRequest(
        host, "admin", BSON("isMaster" << 1), nullptr, kCheckTimeout);
    request.sslMode = _set->setUri.getSSLMode();
    auto status =
        _set->executor
            ->scheduleRemoteCommand(
                std::move(request),
                [copy = *this, host, timer = Timer()](
                    const executor::TaskExecutor::RemoteCommandCallbackArgs& result) mutable {
                    stdx::lock_guard lk(copy._set->mutex);
                    // Ignore the reply and return if we are no longer the current scan. This might
                    // happen if it was decided that the host we were contacting isn't part of the
                    // set.
                    if (copy._scan != copy._set->currentScan) {
                        return;
                    }

                    // ensure that isMaster calls occur at most 500ms after the previous call ended
                    if (auto node = copy._set->findNode(host)) {
                        node->nextPossibleIsMasterCall =
                            copy._set->executor->now() + Milliseconds(500);
                    }

                    if (result.response.isOK()) {
                        // Not using result.response.elapsedMillis because higher precision is
                        // useful for computing the rolling average.
                        copy.receivedIsMaster(host, timer.micros(), result.response.data);
                    } else {
                        copy.failedHost(host, result.response.status);
                    }

                    // This reply may have discovered new hosts to contact so we need to schedule
                    // them.
                    copy.scheduleNetworkRequests();
                })
            .getStatus();

    if (!status.isOK()) {
        failedHost(host, status);
        // This is only called from scheduleNetworkRequests() which will still be looping, so we
        // don't need to call it here after updating the state.
    }
}

Refresher::NextStep Refresher::getNextStep() {
    // No longer the current scan
    if (_scan != _set->currentScan) {
        return NextStep(NextStep::DONE);
    }

    // Wait for all dispatched hosts to return before trying any fallback hosts.
    if (_scan->hostsToScan.empty() && !_scan->waitingFor.empty()) {
        return NextStep(NextStep::WAIT);
    }

    // If we haven't yet found a master, try contacting unconfirmed hosts
    if (_scan->hostsToScan.empty() && !_scan->foundUpMaster) {
        _scan->enqueAllUntriedHosts(_scan->possibleNodes, _set->rand);
        _scan->possibleNodes.clear();
    }

    if (_scan->hostsToScan.empty()) {
        // We've tried all hosts we can, so nothing more to do in this round.
        if (!_scan->foundUpMaster) {
            LOGV2_WARNING(
                24089, "Unable to reach primary for set {set_name}", "set_name"_attr = _set->name);

            // Since we've talked to everyone we could but still didn't find a primary, we
            // do the best we can, and assume all unconfirmedReplies are actually from nodes
            // in the set (we've already confirmed that they think they are). This is
            // important since it allows us to bootstrap to a usable state even if we are
            // unable to talk to a master while starting up. As soon as we are able to
            // contact a master, we will remove any nodes that it doesn't think are part of
            // the set, undoing the damage we cause here.

            // NOTE: we don't modify seedNodes or notify about set membership change in this
            // case since it hasn't been confirmed by a master.
            for (UnconfirmedReplies::iterator it = _scan->unconfirmedReplies.begin();
                 it != _scan->unconfirmedReplies.end();
                 ++it) {
                _set->findOrCreateNode(it->first)->update(it->second);
            }

            auto connStr = _set->possibleConnectionString();
            if (connStr != _set->workingConnStr) {
                _set->workingConnStr = std::move(connStr);
                _set->notifier->onPossibleSet(_set->workingConnStr);
            }

            // If at some point we care about lacking a primary, on it here
            _set->lastSeenMaster = {};
        }

        if (_scan->foundAnyUpNodes) {
            _set->consecutiveFailedScans = 0;
        } else {
            auto nScans = _set->consecutiveFailedScans++;
            if (nScans <= 10 || nScans % 10 == 0) {
                LOGV2(24073,
                      "Cannot reach any nodes for set {set_name}. Please check network "
                      "connectivity and the status of the set. This has happened for "
                      "{set_consecutiveFailedScans} checks in a row.",
                      "set_name"_attr = _set->name,
                      "set_consecutiveFailedScans"_attr = _set->consecutiveFailedScans);
            }
        }

        // Makes sure all other Refreshers in this round return DONE
        _set->currentScan.reset();
        _set->notify();

        LOGV2_DEBUG(24074,
                    1,
                    "Refreshing replica set {set_name} took {scan_timer_millis}ms",
                    "set_name"_attr = _set->name,
                    "scan_timer_millis"_attr = _scan->timer.millis());

        return NextStep(NextStep::DONE);
    }

    // Pop and return the next hostToScan.
    HostAndPort host = _scan->hostsToScan.front();
    _scan->hostsToScan.pop_front();
    _scan->waitingFor.insert(host);
    _scan->triedHosts.insert(host);

    return NextStep(NextStep::CONTACT_HOST, host);
}

void Refresher::receivedIsMaster(const HostAndPort& from,
                                 int64_t latencyMicros,
                                 const BSONObj& replyObj) {
    _scan->waitingFor.erase(from);

    const IsMasterReply reply(from, latencyMicros, replyObj);

    // Handle various failure cases
    if (!reply.ok) {
        failedHost(from, {ErrorCodes::CommandFailed, "Failed to execute 'ismaster' command"});
        return;
    }

    if (reply.setName != _set->name) {
        if (reply.raw["isreplicaset"].trueValue()) {
            // The reply came from a node in the state referred to as RSGhost in the SDAM
            // spec. RSGhost corresponds to either REMOVED or STARTUP member states. In any event,
            // if a reply from a ghost offers a list of possible other members of the replica set,
            // and if this refresher has yet to find the replica set master, we add hosts listed in
            // the reply to the list of possible replica set members.
            if (!_scan->foundUpMaster) {
                _scan->possibleNodes.insert(reply.members.begin(), reply.members.end());
            }
        } else {
            LOGV2_ERROR(24091,
                        "replset name mismatch: expected \"{set_name}\", but remote node {from} "
                        "has replset name \"{reply_setName}\", ismaster: {replyObj}",
                        "set_name"_attr = _set->name,
                        "from"_attr = from,
                        "reply_setName"_attr = reply.setName,
                        "replyObj"_attr = replyObj);
        }

        failedHost(from,
                   {ErrorCodes::InconsistentReplicaSetNames,
                    str::stream() << "Target replica set name " << reply.setName
                                  << " does not match the monitored set name " << _set->name});
        return;
    }

    if (reply.isMaster) {
        Status status = receivedIsMasterFromMaster(from, reply);
        if (!status.isOK()) {
            failedHost(from, status);
            return;
        }
    }

    if (_scan->foundUpMaster) {
        // We only update a Node if a master has confirmed it is in the set.
        _set->updateNodeIfInNodes(reply);
    } else {
        // Populate possibleNodes.
        _scan->possibleNodes.insert(reply.members.begin(), reply.members.end());
        _scan->unconfirmedReplies[from] = reply;
    }

    // _set->nodes may still not have any nodes with isUp==true, but we have at least found a
    // connectible host that is that claims to be in the set.
    _scan->foundAnyUpNodes = true;

    _set->notify();

    if (kDebugBuild)
        _set->checkInvariants();
}

void Refresher::failedHost(const HostAndPort& host, const Status& status) {
    _scan->waitingFor.erase(host);

    Node* node = _set->findNode(host);
    if (node)
        node->markFailed(status);

    if (_scan->waitingFor.empty()) {
        // If this was the last host that needed a response, we should notify the SetState so that
        // we can fail any waiters that have timed out.
        _set->notify();
    }
}

void Refresher::startNewScan() {
    // The heuristics we use in deciding the order to contact hosts are designed to find a
    // master as quickly as possible. This is because we can't use any hosts we find until
    // we either get the latest set of members from a master or talk to all possible hosts
    // without finding a master.

    // TODO It might make sense to check down nodes first if the last seen master is still
    // marked as up.

    _scan = std::make_shared<ScanState>();
    _set->currentScan = _scan;

    int upNodes = 0;
    for (Nodes::const_iterator it(_set->nodes.begin()), end(_set->nodes.end()); it != end; ++it) {
        if (it->isUp) {
            // _scan the nodes we think are up first
            _scan->hostsToScan.push_front(it->host);
            upNodes++;
        } else {
            _scan->hostsToScan.push_back(it->host);
        }
    }

    // shuffle the queue, but keep "up" nodes at the front
    std::shuffle(
        _scan->hostsToScan.begin(), _scan->hostsToScan.begin() + upNodes, _set->rand.urbg());
    std::shuffle(_scan->hostsToScan.begin() + upNodes, _scan->hostsToScan.end(), _set->rand.urbg());

    if (!_set->lastSeenMaster.empty()) {
        // move lastSeenMaster to front of queue
        std::stable_partition(
            _scan->hostsToScan.begin(), _scan->hostsToScan.end(), HostIs(_set->lastSeenMaster));
    }
}

Status Refresher::receivedIsMasterFromMaster(const HostAndPort& from, const IsMasterReply& reply) {
    invariant(reply.isMaster);

    // Reject if config version is older. This is for backwards compatibility with nodes in pv0
    // since they don't have the same ordering with pv1 electionId.
    if (reply.configVersion < _set->configVersion) {
        return {
            ErrorCodes::NotMaster,
            str::stream() << "Node " << from << " believes it is primary, but its config version "
                          << reply.configVersion << " is older than the most recent config version "
                          << _set->configVersion};
    }

    if (reply.electionId.isSet()) {
        // ElectionIds are only comparable if they are of the same protocol version. However, since
        // isMaster has no protocol version field, we use the configVersion instead. This works
        // because configVersion needs to be incremented whenever the protocol version is changed.
        if (reply.configVersion == _set->configVersion && _set->maxElectionId.isSet() &&
            _set->maxElectionId.compare(reply.electionId) > 0) {
            return {
                ErrorCodes::NotMaster,
                str::stream() << "Node " << from << " believes it is primary, but its election id "
                              << reply.electionId << " is older than the most recent election id "
                              << _set->maxElectionId};
        }

        _set->maxElectionId = reply.electionId;
    }

    _set->configVersion = reply.configVersion;

    // Mark all nodes as not master. We will mark ourself as master before releasing the lock.
    // NOTE: we use a "last-wins" policy if multiple hosts claim to be master.
    for (size_t i = 0; i < _set->nodes.size(); i++) {
        _set->nodes[i].isMaster = false;
    }

    // Check if the master agrees with our current list of nodes.
    // REMINDER: both _set->nodes and reply.members are sorted.
    if (_set->nodes.size() != reply.members.size() ||
        !std::equal(_set->nodes.begin(), _set->nodes.end(), reply.members.begin(), hostsEqual)) {
        LOGV2_DEBUG(24075,
                    2,
                    "Adjusting nodes in our view of replica set {set_name} based on master reply: "
                    "{reply_raw}",
                    "set_name"_attr = _set->name,
                    "reply_raw"_attr = redact(reply.raw));

        // remove non-members from _set->nodes
        _set->nodes.erase(
            std::remove_if(_set->nodes.begin(), _set->nodes.end(), HostNotIn(reply.members)),
            _set->nodes.end());

        // add new members to _set->nodes
        for (auto& host : reply.members) {
            _set->findOrCreateNode(host);
        }

        // replace hostToScan queue with untried normal hosts. can both add and remove
        // hosts from the queue.
        _scan->hostsToScan.clear();
        _scan->enqueAllUntriedHosts(reply.members, _set->rand);

        if (!_scan->waitingFor.empty()) {
            // make sure we don't wait for any hosts that aren't considered members
            std::set<HostAndPort> newWaitingFor;
            std::set_intersection(reply.members.begin(),
                                  reply.members.end(),
                                  _scan->waitingFor.begin(),
                                  _scan->waitingFor.end(),
                                  std::inserter(newWaitingFor, newWaitingFor.end()));
            _scan->waitingFor.swap(newWaitingFor);
        }
    }

    bool changedHosts = reply.members != _set->seedNodes;
    bool changedPrimary = reply.host != _set->lastSeenMaster;
    if (changedHosts || changedPrimary) {
        ++_set->seedGen;
        _set->seedNodes = reply.members;
        _set->seedConnStr = _set->confirmedConnectionString();

        // LogLevel can be pretty low, since replica set reconfiguration should be pretty rare
        // and we want to record our changes
        LOGV2(24076,
              "Confirmed replica set for {set_name} is {set_seedConnStr}",
              "set_name"_attr = _set->name,
              "set_seedConnStr"_attr = _set->seedConnStr);

        _set->notifier->onConfirmedSet(_set->seedConnStr, reply.host, reply.passives);
    }

    // Update our working string
    _set->workingConnStr = _set->seedConnStr;

    // Update other nodes's information based on replies we've already seen
    for (UnconfirmedReplies::iterator it = _scan->unconfirmedReplies.begin();
         it != _scan->unconfirmedReplies.end();
         ++it) {
        // this ignores replies from hosts not in _set->nodes (as modified above)
        _set->updateNodeIfInNodes(it->second);
    }
    _scan->unconfirmedReplies.clear();

    _scan->foundUpMaster = true;
    _set->lastSeenMaster = reply.host;

    return Status::OK();
}

void IsMasterReply::parse(const BSONObj& obj) {
    try {
        raw = obj.getOwned();  // don't use obj again after this line

        ok = raw["ok"].trueValue();
        if (!ok)
            return;

        setName = raw["setName"].str();
        hidden = raw["hidden"].trueValue();
        secondary = raw["secondary"].trueValue();

        minWireVersion = raw["minWireVersion"].numberInt();
        maxWireVersion = raw["maxWireVersion"].numberInt();

        // hidden nodes can't be master, even if they claim to be.
        isMaster = !hidden && raw["ismaster"].trueValue();

        if (isMaster && raw.hasField("electionId")) {
            electionId = raw["electionId"].OID();
        }

        configVersion = raw["setVersion"].numberInt();

        const string primaryString = raw["primary"].str();
        primary = primaryString.empty() ? HostAndPort() : HostAndPort(primaryString);

        // both hosts and passives, but not arbiters, are considered "normal hosts"
        members.clear();
        BSONForEach(host, raw.getObjectField("hosts")) {
            members.insert(HostAndPort(host.String()));
        }
        BSONForEach(host, raw.getObjectField("passives")) {
            members.insert(HostAndPort(host.String()));
            passives.insert(HostAndPort(host.String()));
        }

        tags = raw.getObjectField("tags");
        BSONObj lastWriteField = raw.getObjectField("lastWrite");
        if (!lastWriteField.isEmpty()) {
            if (auto lastWrite = lastWriteField["lastWriteDate"]) {
                lastWriteDate = lastWrite.date();
            }

            uassertStatusOK(bsonExtractOpTimeField(lastWriteField, "opTime", &opTime));
        }
    } catch (const std::exception& e) {
        ok = false;
        LOGV2(24077,
              "exception while parsing isMaster reply: {e_what} {obj}",
              "e_what"_attr = e.what(),
              "obj"_attr = obj);
    }
}

Node::Node(const HostAndPort& host) : host(host), latencyMicros(unknownLatency) {}

void Node::markFailed(const Status& status) {
    if (isUp) {
        LOGV2(24078,
              "Marking host {host} as failed{causedBy_status}",
              "host"_attr = host,
              "causedBy_status"_attr = causedBy(redact(status)));

        isUp = false;
    }

    isMaster = false;
}

bool Node::matches(const ReadPreference pref) const {
    if (!isUp) {
        LOGV2_DEBUG(24079, 3, "Host {host} is not up", "host"_attr = host);
        return false;
    }

    LOGV2_DEBUG(24080,
                3,
                "Host {host} is {isMaster_primary_not_primary}",
                "host"_attr = host,
                "isMaster_primary_not_primary"_attr = (isMaster ? "primary" : "not primary"));
    if (pref == ReadPreference::PrimaryOnly) {
        return isMaster;
    }

    if (pref == ReadPreference::SecondaryOnly) {
        return !isMaster;
    }

    return true;
}

bool Node::matches(const BSONObj& tag) const {
    BSONForEach(tagCriteria, tag) {
        if (SimpleBSONElementComparator::kInstance.evaluate(
                this->tags[tagCriteria.fieldNameStringData()] != tagCriteria)) {
            return false;
        }
    }

    return true;
}

void Node::update(const IsMasterReply& reply) {
    invariant(host == reply.host);
    invariant(reply.ok);

    LOGV2_DEBUG(24081,
                3,
                "Updating host {host} based on ismaster reply: {reply_raw}",
                "host"_attr = host,
                "reply_raw"_attr = reply.raw);

    // Nodes that are hidden or neither master or secondary are considered down since we can't
    // send any operations to them.
    isUp = !reply.hidden && (reply.isMaster || reply.secondary);
    isMaster = reply.isMaster;

    minWireVersion = reply.minWireVersion;
    maxWireVersion = reply.maxWireVersion;

    // save a copy if unchanged
    if (!tags.binaryEqual(reply.tags))
        tags = reply.tags.getOwned();

    if (reply.latencyMicros >= 0) {  // TODO upper bound?
        if (latencyMicros == unknownLatency) {
            latencyMicros = reply.latencyMicros;
        } else {
            // update latency with smoothed moving average (1/4th the delta)
            latencyMicros += (reply.latencyMicros - latencyMicros) / 4;
        }
    }

    LOGV2_DEBUG(24082,
                3,
                "Updating {host} lastWriteDate to {reply_lastWriteDate}",
                "host"_attr = host,
                "reply_lastWriteDate"_attr = reply.lastWriteDate);
    lastWriteDate = reply.lastWriteDate;

    LOGV2_DEBUG(24083,
                3,
                "Updating {host} opTime to {reply_opTime}",
                "host"_attr = host,
                "reply_opTime"_attr = reply.opTime);
    opTime = reply.opTime;
    lastWriteDateUpdateTime = Date_t::now();
}

SetState::SetState(const MongoURI& uri,
                   ReplicaSetChangeNotifier* notifier,
                   executor::TaskExecutor* executor)
    : setUri(std::move(uri)),
      name(setUri.getSetName()),
      notifier(notifier),
      executor(executor),
      seedNodes(setUri.getServers().begin(), setUri.getServers().end()),
      latencyThresholdMicros(serverGlobalParams.defaultLocalThresholdMillis * int64_t(1000)),
      rand(std::random_device()()),
      refreshPeriod(getDefaultRefreshPeriod()) {
    uassert(13642, "Replica set seed list can't be empty", !seedNodes.empty());

    if (name.empty())
        LOGV2_WARNING(24090,
                      "Replica set name empty, first node: {seedNodes_begin}",
                      "seedNodes_begin"_attr = *(seedNodes.begin()));

    // This adds the seed hosts to nodes, but they aren't usable for anything except seeding a
    // scan until we start a scan and either find a master or contact all hosts without finding
    // one.
    // WARNING: if seedNodes is ever changed to not imply sorted iteration, you will need to
    // sort nodes after this loop.
    for (auto&& addr : seedNodes) {
        nodes.push_back(Node(addr));

        if (addr.host()[0] == '$') {
            invariant(isMocked || &addr == &*seedNodes.begin());  // Can't mix and match.
            isMocked = true;
        } else {
            invariant(!isMocked);  // Can't mix and match.
        }
    }

    if (kDebugBuild)
        checkInvariants();
}

HostAndPort SetState::getMatchingHost(const ReadPreferenceSetting& criteria) const {
    auto hosts = getMatchingHosts(criteria);

    if (hosts.empty()) {
        return HostAndPort();
    }

    return hosts[0];
}

std::vector<HostAndPort> SetState::getMatchingHosts(const ReadPreferenceSetting& criteria) const {
    switch (criteria.pref) {
        // "Prefered" read preferences are defined in terms of other preferences
        case ReadPreference::PrimaryPreferred: {
            std::vector<HostAndPort> out =
                getMatchingHosts(ReadPreferenceSetting(ReadPreference::PrimaryOnly, criteria.tags));
            // NOTE: the spec says we should use the primary even if tags don't match
            if (!out.empty())
                return out;
            return getMatchingHosts(ReadPreferenceSetting(
                ReadPreference::SecondaryOnly, criteria.tags, criteria.maxStalenessSeconds));
        }

        case ReadPreference::SecondaryPreferred: {
            std::vector<HostAndPort> out = getMatchingHosts(ReadPreferenceSetting(
                ReadPreference::SecondaryOnly, criteria.tags, criteria.maxStalenessSeconds));
            if (!out.empty())
                return out;
            // NOTE: the spec says we should use the primary even if tags don't match
            return getMatchingHosts(
                ReadPreferenceSetting(ReadPreference::PrimaryOnly, criteria.tags));
        }

        case ReadPreference::PrimaryOnly: {
            // NOTE: isMaster implies isUp
            Nodes::const_iterator it = std::find_if(nodes.begin(), nodes.end(), isMaster);
            if (it == nodes.end())
                return {};
            return {it->host};
        }

        // The difference between these is handled by Node::matches
        case ReadPreference::SecondaryOnly:
        case ReadPreference::Nearest: {
            std::function<bool(const Node&)> matchNode = [](const Node& node) -> bool {
                return true;
            };
            // build comparator
            if (criteria.maxStalenessSeconds.count()) {
                auto masterIt = std::find_if(nodes.begin(), nodes.end(), isMaster);
                if (masterIt == nodes.end() || !masterIt->lastWriteDate.toMillisSinceEpoch()) {
                    auto writeDateCmp = [](const Node* a, const Node* b) -> bool {
                        return a->lastWriteDate < b->lastWriteDate;
                    };
                    // use only non failed nodes
                    std::vector<const Node*> upNodes;
                    for (auto nodeIt = nodes.begin(); nodeIt != nodes.end(); ++nodeIt) {
                        if (nodeIt->isUp && nodeIt->lastWriteDate.toMillisSinceEpoch()) {
                            upNodes.push_back(&(*nodeIt));
                        }
                    }
                    auto latestSecNode =
                        std::max_element(upNodes.begin(), upNodes.end(), writeDateCmp);
                    if (latestSecNode == upNodes.end()) {
                        matchNode = [](const Node& node) -> bool { return false; };
                    } else {
                        Date_t maxWriteTime = (*latestSecNode)->lastWriteDate;
                        matchNode = [=](const Node& node) -> bool {
                            return duration_cast<Seconds>(maxWriteTime - node.lastWriteDate) +
                                refreshPeriod <=
                                criteria.maxStalenessSeconds;
                        };
                    }
                } else {
                    Seconds primaryStaleness = duration_cast<Seconds>(
                        masterIt->lastWriteDateUpdateTime - masterIt->lastWriteDate);
                    matchNode = [=](const Node& node) -> bool {
                        return duration_cast<Seconds>(node.lastWriteDateUpdateTime -
                                                      node.lastWriteDate) -
                            primaryStaleness + refreshPeriod <=
                            criteria.maxStalenessSeconds;
                    };
                }
            }

            std::vector<const Node*> allMatchingNodes;
            BSONForEach(tagElem, criteria.tags.getTagBSON()) {
                uassert(16358, "Tags should be a BSON object", tagElem.isABSONObj());
                BSONObj tag = tagElem.Obj();

                std::vector<const Node*> matchingNodes;
                for (size_t i = 0; i < nodes.size(); i++) {
                    if (nodes[i].matches(criteria.pref) && nodes[i].matches(tag) &&
                        matchNode(nodes[i])) {
                        matchingNodes.push_back(&nodes[i]);
                    }
                }

                // Only consider nodes that satisfy the minOpTime
                if (!criteria.minOpTime.isNull()) {
                    std::sort(matchingNodes.begin(), matchingNodes.end(), opTimeGreater);
                    for (size_t i = 0; i < matchingNodes.size(); i++) {
                        if (matchingNodes[i]->opTime < criteria.minOpTime) {
                            if (i == 0) {
                                // If no nodes satisfy the minOpTime criteria, we ignore the
                                // minOpTime requirement.
                                break;
                            }
                            matchingNodes.erase(matchingNodes.begin() + i, matchingNodes.end());
                            break;
                        }
                    }
                }

                allMatchingNodes.insert(
                    allMatchingNodes.end(), matchingNodes.begin(), matchingNodes.end());
            }

            // don't do more complicated selection if not needed
            if (allMatchingNodes.empty()) {
                return {};
            }
            if (allMatchingNodes.size() == 1) {
                return {allMatchingNodes.front()->host};
            }

            // If there are multiple nodes satisfying the minOpTime, next order by latency
            // and don't consider hosts further than a threshold from the closest.
            std::sort(allMatchingNodes.begin(), allMatchingNodes.end(), compareLatencies);
            for (size_t i = 1; i < allMatchingNodes.size(); i++) {
                int64_t distance =
                    allMatchingNodes[i]->latencyMicros - allMatchingNodes[0]->latencyMicros;
                if (distance >= latencyThresholdMicros) {
                    // this node and all remaining ones are too far away
                    allMatchingNodes.erase(allMatchingNodes.begin() + i, allMatchingNodes.end());
                    break;
                }
            }

            std::vector<HostAndPort> hosts;
            std::transform(allMatchingNodes.begin(),
                           allMatchingNodes.end(),
                           std::back_inserter(hosts),
                           [](const auto& node) { return node->host; });

            // Note that the host list is only deterministic (or random) for the first node.
            // The rest of the list is in matchingNodes order (latency) with one element swapped
            // for the first element.
            if (auto bestHostIdx = useDeterministicHostSelection ? roundRobin++ % hosts.size()
                                                                 : rand.nextInt32(hosts.size())) {
                using std::swap;
                swap(hosts[0], hosts[bestHostIdx]);
            }

            return hosts;
        }

        default:
            uassert(16337, "Unknown read preference", false);
            break;
    }
}

Node* SetState::findNode(const HostAndPort& host) {
    const Nodes::iterator it = std::lower_bound(nodes.begin(), nodes.end(), host, compareHosts);
    if (it == nodes.end() || it->host != host)
        return nullptr;

    return &(*it);
}

Node* SetState::findOrCreateNode(const HostAndPort& host) {
    // This is insertion sort, but N is currently guaranteed to be <= 12 (although this class
    // must function correctly even with more nodes). If we lift that restriction, we may need
    // to consider alternate algorithms.
    Nodes::iterator it = std::lower_bound(nodes.begin(), nodes.end(), host, compareHosts);
    if (it == nodes.end() || it->host != host) {
        LOGV2_DEBUG(24084,
                    2,
                    "Adding node {host} to our view of replica set {name}",
                    "host"_attr = host,
                    "name"_attr = name);
        it = nodes.insert(it, Node(host));
    }
    return &(*it);
}

void SetState::updateNodeIfInNodes(const IsMasterReply& reply) {
    Node* node = findNode(reply.host);
    if (!node) {
        LOGV2_DEBUG(24085,
                    2,
                    "Skipping application of ismaster reply from {reply_host} since it isn't a "
                    "confirmed member of set {name}",
                    "reply_host"_attr = reply.host,
                    "name"_attr = name);
        return;
    }

    node->update(reply);
}

ConnectionString SetState::confirmedConnectionString() const {
    std::vector<HostAndPort> hosts(begin(seedNodes), end(seedNodes));

    return ConnectionString::forReplicaSet(name, std::move(hosts));
}

ConnectionString SetState::possibleConnectionString() const {
    std::vector<HostAndPort> hosts;
    hosts.reserve(nodes.size());

    for (auto& node : nodes) {
        hosts.push_back(node.host);
    }

    return ConnectionString::forReplicaSet(name, std::move(hosts));
}

void SetState::notify() {
    if (!waiters.size()) {
        return;
    }

    const auto cachedNow = now();
    auto shouldQuickFail = areRefreshRetriesDisabledForTest() && !currentScan;

    for (auto it = waiters.begin(); it != waiters.end();) {
        if (isDropped) {
            it->promise.setError({ErrorCodes::ShutdownInProgress,
                                  str::stream() << "ScanningReplicaSetMonitor is shutting down"});
            waiters.erase(it++);
            continue;
        }

        auto match = getMatchingHosts(it->criteria);
        if (!match.empty()) {
            // match;
            it->promise.emplaceValue(std::move(match));
            waiters.erase(it++);
        } else if (it->deadline <= cachedNow) {
            LOGV2_DEBUG(
                24086,
                1,
                "Unable to statisfy read preference {it_criteria} by deadline {it_deadline}",
                "it_criteria"_attr = it->criteria,
                "it_deadline"_attr = it->deadline);
            it->promise.setError(makeUnsatisfedReadPrefError(it->criteria));
            waiters.erase(it++);
        } else if (shouldQuickFail) {
            LOGV2_DEBUG(24087, 1, "Unable to statisfy read preference because tests fail quickly");
            it->promise.setError(makeUnsatisfedReadPrefError(it->criteria));
            waiters.erase(it++);
        } else {
            it++;
        }
    }

    if (waiters.empty()) {
        // No current waiters so we can stop the expedited scanning.
        isExpedited = false;
        rescheduleRefresh(SchedulingStrategy::kCancelPreviousScan);
    }
}

Status SetState::makeUnsatisfedReadPrefError(const ReadPreferenceSetting& criteria) const {
    return Status(ErrorCodes::FailedToSatisfyReadPreference,
                  str::stream() << "Could not find host matching read preference "
                                << criteria.toString() << " for set " << name);
}

void SetState::init() {
    rescheduleRefresh(SchedulingStrategy::kKeepEarlyScan);
    notifier->onFoundSet(name);
}

void SetState::drop() {
    if (std::exchange(isDropped, true)) {
        // If a SetState calls drop() from destruction after the RSMM calls shutdown(), then the
        // RSMM's executor may no longer exist. Thus, only drop once.
        return;
    }

    currentScan.reset();
    notify();

    if (auto handle = std::exchange(refresherHandle, {})) {
        // Cancel our refresh on the way out
        executor->cancel(handle);
    }

    for (auto& node : nodes) {
        if (auto handle = std::exchange(node.scheduledIsMasterHandle, {})) {
            // Cancel any isMasters we had scheduled
            executor->cancel(handle);
        }
    }

    // No point in notifying if we never started
    if (workingConnStr.isValid()) {
        notifier->onDroppedSet(name);
    }
}

void SetState::checkInvariants() const {
    bool foundMaster = false;
    for (size_t i = 0; i < nodes.size(); i++) {
        // no empty hosts
        invariant(!nodes[i].host.empty());

        if (nodes[i].isMaster) {
            // masters must be up
            invariant(nodes[i].isUp);

            // at most one master
            invariant(!foundMaster);
            foundMaster = true;

            // if we have a master it should be the same as lastSeenMaster
            invariant(lastSeenMaster.empty() || nodes[i].host == lastSeenMaster);
        }

        // should never end up with negative latencies
        invariant(nodes[i].latencyMicros >= 0);

        // nodes must be sorted by host with no-dupes
        invariant(i == 0 || (nodes[i - 1].host < nodes[i].host));
    }

    // nodes should be a (non-strict) superset of the seedNodes
    invariant(std::includes(
        nodes.begin(), nodes.end(), seedNodes.begin(), seedNodes.end(), compareHosts));

    if (currentScan) {
        // hostsToScan can't have dups or hosts already in triedHosts.
        std::set<HostAndPort> cantSee = currentScan->triedHosts;
        for (std::deque<HostAndPort>::const_iterator it = currentScan->hostsToScan.begin();
             it != currentScan->hostsToScan.end();
             ++it) {
            invariant(!cantSee.count(*it));
            cantSee.insert(*it);  // make sure we don't see this again
        }

        // We should only be waitingFor hosts that are in triedHosts
        invariant(std::includes(currentScan->triedHosts.begin(),
                                currentScan->triedHosts.end(),
                                currentScan->waitingFor.begin(),
                                currentScan->waitingFor.end()));

        // We should only have unconfirmedReplies if we haven't found a master yet
        invariant(!currentScan->foundUpMaster || currentScan->unconfirmedReplies.empty());
    }
}

template <typename Container>
void ScanState::enqueAllUntriedHosts(const Container& container, PseudoRandom& rand) {
    invariant(hostsToScan.empty());  // because we don't try to dedup hosts already in the queue.

    // no std::copy_if before c++11
    for (typename Container::const_iterator it(container.begin()), end(container.end()); it != end;
         ++it) {
        if (!triedHosts.count(*it)) {
            hostsToScan.push_back(*it);
        }
    }
    std::shuffle(hostsToScan.begin(), hostsToScan.end(), rand.urbg());
}

void ScanState::retryAllTriedHosts(PseudoRandom& rand) {
    invariant(hostsToScan.empty());  // because we don't try to dedup hosts already in the queue.
    // Move hosts that are in triedHosts but not in waitingFor from triedHosts to hostsToScan.
    std::set_difference(triedHosts.begin(),
                        triedHosts.end(),
                        waitingFor.begin(),
                        waitingFor.end(),
                        std::inserter(hostsToScan, hostsToScan.end()));
    std::shuffle(hostsToScan.begin(), hostsToScan.end(), rand.urbg());
    triedHosts = waitingFor;
}

void ScanState::markHostsToScanAsTried() noexcept {
    while (!hostsToScan.empty()) {
        auto host = hostsToScan.front();
        hostsToScan.pop_front();
        /**
         * Mark the popped host as tried to avoid deleting hosts in multiple points.
         * This emulates the final effect of Refresher::getNextStep() on the set.
         */
        triedHosts.insert(host);
    }
}
}  // namespace mongo
