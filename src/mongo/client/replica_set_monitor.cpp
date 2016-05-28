/*    Copyright 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/replica_set_monitor.h"

#include <algorithm>
#include <limits>

#include "mongo/client/connpool.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/replica_set_monitor_internal.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/mutex.h"  // for StaticObserver
#include "mongo/util/debug_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/static_observer.h"
#include "mongo/util/string_map.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::shared_ptr;
using std::numeric_limits;
using std::set;
using std::string;
using std::vector;

namespace {

// Pull nested types to top-level scope
typedef ReplicaSetMonitor::IsMasterReply IsMasterReply;
typedef ReplicaSetMonitor::ScanState ScanState;
typedef ReplicaSetMonitor::ScanStatePtr ScanStatePtr;
typedef ReplicaSetMonitor::SetState SetState;
typedef ReplicaSetMonitor::SetStatePtr SetStatePtr;
typedef ReplicaSetMonitor::Refresher Refresher;
typedef ScanState::UnconfirmedReplies UnconfirmedReplies;
typedef SetState::Node Node;
typedef SetState::Nodes Nodes;
using executor::TaskExecutor;
using CallbackArgs = TaskExecutor::CallbackArgs;
using CallbackHandle = TaskExecutor::CallbackHandle;

const double socketTimeoutSecs = 5;

// Intentionally chosen to compare worse than all known latencies.
const int64_t unknownLatency = numeric_limits<int64_t>::max();

const ReadPreferenceSetting kPrimaryOnlyReadPreference(ReadPreference::PrimaryOnly, TagSet());
const Milliseconds kFindHostMaxBackOffTime(500);

// TODO: Move to ReplicaSetMonitorManager
ReplicaSetMonitor::ConfigChangeHook asyncConfigChangeHook;
ReplicaSetMonitor::ConfigChangeHook syncConfigChangeHook;

StaticObserver staticObserver;

//
// Helpers for stl algorithms
//

bool isMaster(const Node& node) {
    return node.isMaster;
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
/**
 * Replica set refresh period on the task executor.
 */
const Seconds kRefreshPeriod(30);


}  // namespace

// At 1 check every 10 seconds, 30 checks takes 5 minutes
std::atomic<int> ReplicaSetMonitor::maxConsecutiveFailedChecks(30);  // NOLINT

// If we cannot find a host after 15 seconds of refreshing, give up
const Seconds ReplicaSetMonitor::kDefaultFindHostTimeout(15);

// Defaults to random selection as required by the spec
bool ReplicaSetMonitor::useDeterministicHostSelection = false;

ReplicaSetMonitor::ReplicaSetMonitor(StringData name, const std::set<HostAndPort>& seeds)
    : _state(std::make_shared<SetState>(name, seeds)),
      _executor(globalRSMonitorManager.getExecutor()) {}

void ReplicaSetMonitor::init() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_executor);
    std::weak_ptr<ReplicaSetMonitor> that(shared_from_this());
    auto status = _executor->scheduleWork([=](const CallbackArgs& cbArgs) {
        if (auto ptr = that.lock()) {
            ptr->_refresh(cbArgs);
        }
    });

    if (status.getStatus() == ErrorCodes::ShutdownInProgress) {
        LOG(1) << "Couldn't schedule refresh for " << getName()
               << ". Executor shutdown in progress";
        return;
    }

    if (!status.isOK()) {
        severe() << "Can't start refresh for replica set " << getName()
                 << causedBy(status.getStatus());
        fassertFailed(40139);
    }

    _refresherHandle = status.getValue();
}

ReplicaSetMonitor::~ReplicaSetMonitor() {
    // need this lock because otherwise can get race with scheduling in _refresh
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (!_refresherHandle || !_executor) {
        return;
    }

    _executor->cancel(_refresherHandle);
    // Note: calling _executor->wait(_refresherHandle); from the dispatcher thread will cause hang
    // Its ok not to call it because the d-tor is called only when the last owning pointer goes out
    // of scope, so as taskExecutor queue holds a weak pointer to RSM it will not be able to get a
    // task to execute eliminating the need to call method "wait".
    //
    _refresherHandle = {};
}

void ReplicaSetMonitor::_refresh(const CallbackArgs& cbArgs) {
    if (!cbArgs.status.isOK()) {
        return;
    }

    Timer t;
    startOrContinueRefresh().refreshAll();
    LOG(1) << "Refreshing replica set " << getName() << " took " << t.millis() << " msec";

    if (!isSetUsable()) {
        log() << "Stopping periodic monitoring of set " << getName()
              << " because none of the hosts could be contacted for an extended period of "
                 "time.";

        ReplicaSetMonitor::remove(getName());
        return;
    }

    {
        // reschedule itself
        invariant(_executor);
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        std::weak_ptr<ReplicaSetMonitor> that(shared_from_this());
        auto status = _executor->scheduleWorkAt(_executor->now() + kRefreshPeriod,
                                                [=](const CallbackArgs& cbArgs) {
                                                    if (auto ptr = that.lock()) {
                                                        ptr->_refresh(cbArgs);
                                                    }
                                                });

        if (status.getStatus() == ErrorCodes::ShutdownInProgress) {
            LOG(1) << "Cant schedule refresh for " << getName()
                   << ". Executor shutdown in progress";
            return;
        }

        if (!status.isOK()) {
            severe() << "Can't continue refresh for replica set " << getName() << " due to "
                     << status.getStatus().toString();
            fassertFailed(40140);
        }

        _refresherHandle = status.getValue();
    }
}

StatusWith<HostAndPort> ReplicaSetMonitor::getHostOrRefresh(const ReadPreferenceSetting& criteria,
                                                            Milliseconds maxWait) {
    {
        // Fast path, for the failure-free case
        stdx::lock_guard<stdx::mutex> lk(_state->mutex);
        HostAndPort out = _state->getMatchingHost(criteria);
        if (!out.empty())
            return out;
    }

    const auto startTimeMs = Date_t::now();

    while (true) {
        // We might not have found any matching hosts due to the scan, which just completed may have
        // seen stale data from before we joined. Therefore we should participate in a new scan to
        // make sure all hosts are contacted at least once (possibly by other threads) before this
        // function gives up.
        Refresher refresher(startOrContinueRefresh());

        HostAndPort out = refresher.refreshUntilMatches(criteria);
        if (!out.empty())
            return out;

        if (!isSetUsable()) {
            return Status(ErrorCodes::ReplicaSetNotFound,
                          str::stream() << "None of the hosts for replica set " << getName()
                                        << " could be contacted.");
        }

        const Milliseconds remaining = maxWait - (Date_t::now() - startTimeMs);

        if (remaining < kFindHostMaxBackOffTime) {
            break;
        }

        // Back-off so we don't spam the replica set hosts too much
        sleepFor(kFindHostMaxBackOffTime);
    }

    return Status(ErrorCodes::FailedToSatisfyReadPreference,
                  str::stream() << "could not find host matching read preference "
                                << criteria.toString()
                                << " for set "
                                << getName());
}

HostAndPort ReplicaSetMonitor::getMasterOrUassert() {
    return uassertStatusOK(getHostOrRefresh(kPrimaryOnlyReadPreference));
}

Refresher ReplicaSetMonitor::startOrContinueRefresh() {
    stdx::lock_guard<stdx::mutex> lk(_state->mutex);

    Refresher out(_state);
    DEV _state->checkInvariants();
    return out;
}

void ReplicaSetMonitor::failedHost(const HostAndPort& host) {
    stdx::lock_guard<stdx::mutex> lk(_state->mutex);
    Node* node = _state->findNode(host);
    if (node)
        node->markFailed();
    DEV _state->checkInvariants();
}

bool ReplicaSetMonitor::isPrimary(const HostAndPort& host) const {
    stdx::lock_guard<stdx::mutex> lk(_state->mutex);
    Node* node = _state->findNode(host);
    return node ? node->isMaster : false;
}

bool ReplicaSetMonitor::isHostUp(const HostAndPort& host) const {
    stdx::lock_guard<stdx::mutex> lk(_state->mutex);
    Node* node = _state->findNode(host);
    return node ? node->isUp : false;
}

int ReplicaSetMonitor::getMinWireVersion() const {
    stdx::lock_guard<stdx::mutex> lk(_state->mutex);
    int minVersion = 0;
    for (const auto& host : _state->nodes) {
        if (host.isUp) {
            minVersion = std::max(minVersion, host.minWireVersion);
        }
    }

    return minVersion;
}

int ReplicaSetMonitor::getMaxWireVersion() const {
    stdx::lock_guard<stdx::mutex> lk(_state->mutex);
    int maxVersion = std::numeric_limits<int>::max();
    for (const auto& host : _state->nodes) {
        if (host.isUp) {
            maxVersion = std::min(maxVersion, host.maxWireVersion);
        }
    }

    return maxVersion;
}

bool ReplicaSetMonitor::isSetUsable() const {
    stdx::lock_guard<stdx::mutex> lk(_state->mutex);
    return _state->isUsable();
}

std::string ReplicaSetMonitor::getName() const {
    // name is const so don't need to lock
    return _state->name;
}

std::string ReplicaSetMonitor::getServerAddress() const {
    stdx::lock_guard<stdx::mutex> lk(_state->mutex);
    return _state->getConfirmedServerAddress();
}

bool ReplicaSetMonitor::contains(const HostAndPort& host) const {
    stdx::lock_guard<stdx::mutex> lk(_state->mutex);
    return _state->seedNodes.count(host);
}

void ReplicaSetMonitor::createIfNeeded(const string& name, const set<HostAndPort>& servers) {
    globalRSMonitorManager.getOrCreateMonitor(
        ConnectionString::forReplicaSet(name, vector<HostAndPort>(servers.begin(), servers.end())));
}

shared_ptr<ReplicaSetMonitor> ReplicaSetMonitor::get(const std::string& name) {
    return globalRSMonitorManager.getMonitor(name);
}

void ReplicaSetMonitor::remove(const string& name) {
    globalRSMonitorManager.removeMonitor(name);

    // Kill all pooled ReplicaSetConnections for this set. They will not function correctly
    // after we kill the ReplicaSetMonitor.
    globalConnPool.removeHost(name);
}

void ReplicaSetMonitor::setAsynchronousConfigChangeHook(ConfigChangeHook hook) {
    invariant(!asyncConfigChangeHook);
    asyncConfigChangeHook = hook;
}

void ReplicaSetMonitor::setSynchronousConfigChangeHook(ConfigChangeHook hook) {
    invariant(!syncConfigChangeHook);
    syncConfigChangeHook = hook;
}

// TODO move to correct order with non-statics before pushing
void ReplicaSetMonitor::appendInfo(BSONObjBuilder& bsonObjBuilder) const {
    stdx::lock_guard<stdx::mutex> lk(_state->mutex);

    // NOTE: the format here must be consistent for backwards compatibility
    BSONArrayBuilder hosts(bsonObjBuilder.subarrayStart("hosts"));
    for (unsigned i = 0; i < _state->nodes.size(); i++) {
        const Node& node = _state->nodes[i];

        BSONObjBuilder builder;
        builder.append("addr", node.host.toString());
        builder.append("ok", node.isUp);
        builder.append("ismaster", node.isMaster);  // intentionally not camelCase
        builder.append("hidden", false);            // we don't keep hidden nodes in the set
        builder.append("secondary", node.isUp && !node.isMaster);

        int32_t pingTimeMillis = 0;
        if (node.latencyMicros / 1000 > numeric_limits<int32_t>::max()) {
            // In particular, Node::unknownLatency does not fit in an int32.
            pingTimeMillis = numeric_limits<int32_t>::max();
        } else {
            pingTimeMillis = node.latencyMicros / 1000;
        }
        builder.append("pingTimeMillis", pingTimeMillis);

        if (!node.tags.isEmpty()) {
            builder.append("tags", node.tags);
        }

        hosts.append(builder.obj());
    }
    hosts.done();
}

void ReplicaSetMonitor::cleanup() {
    globalRSMonitorManager.removeAllMonitors();
    asyncConfigChangeHook = ReplicaSetMonitor::ConfigChangeHook();
    syncConfigChangeHook = ReplicaSetMonitor::ConfigChangeHook();
}

bool ReplicaSetMonitor::isKnownToHaveGoodPrimary() const {
    stdx::lock_guard<stdx::mutex> lk(_state->mutex);

    for (const auto& node : _state->nodes) {
        if (node.isMaster) {
            return true;
        }
    }

    return false;
}

Refresher::Refresher(const SetStatePtr& setState)
    : _set(setState), _scan(setState->currentScan), _startedNewScan(false) {
    if (_scan)
        return;  // participate in in-progress scan

    LOG(2) << "Starting new refresh of replica set " << _set->name;
    _scan = startNewScan(_set.get());
    _set->currentScan = _scan;
    _startedNewScan = true;
}

Refresher::NextStep Refresher::getNextStep() {
    // If the set is faulty, don't try anymore
    if (!_set->isUsable()) {
        return NextStep(NextStep::DONE);
    }

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
            warning() << "No primary detected for set " << _set->name;

            // Since we've talked to everyone we could but still didn't find a primary, we
            // do the best we can, and assume all unconfirmedReplies are actually from nodes
            // in the set (we've already confirmed that they think they are). This is
            // important since it allows us to bootstrap to a usable state even if we are
            // unable to talk to a master while starting up. As soon as we are able to
            // contact a master, we will remove any nodes that it doesn't think are part of
            // the set, undoing the damage we cause here.

            // NOTE: we don't modify seedNodes or notify about set membership change in this
            // case since it hasn't been confirmed by a master.
            const string oldAddr = _set->getUnconfirmedServerAddress();
            for (UnconfirmedReplies::iterator it = _scan->unconfirmedReplies.begin();
                 it != _scan->unconfirmedReplies.end();
                 ++it) {
                _set->findOrCreateNode(it->host)->update(*it);
            }

            const string newAddr = _set->getUnconfirmedServerAddress();
            if (oldAddr != newAddr && syncConfigChangeHook) {
                // Run the syncConfigChangeHook because the ShardRegistry needs to know about any
                // node we might talk to.  Don't run the asyncConfigChangeHook because we don't
                // want to update the seed list stored on the config servers with unconfirmed hosts.
                syncConfigChangeHook(_set->name, _set->getUnconfirmedServerAddress());
            }
        }

        if (_scan->foundAnyUpNodes) {
            _set->consecutiveFailedScans = 0;
        } else {
            _set->consecutiveFailedScans++;
            log() << "All nodes for set " << _set->name << " are down. "
                  << "This has happened for " << _set->consecutiveFailedScans
                  << " checks in a row. Polling will stop after "
                  << maxConsecutiveFailedChecks - _set->consecutiveFailedScans
                  << " more failed checks";
        }

        // Makes sure all other Refreshers in this round return DONE
        _set->currentScan.reset();

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
    // Be careful: all return paths must call either failedHost or cv.notify_all!
    _scan->waitingFor.erase(from);

    const IsMasterReply reply(from, latencyMicros, replyObj);
    // Handle various failure cases
    if (!reply.ok) {
        failedHost(from);
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
                _scan->possibleNodes.insert(reply.normalHosts.begin(), reply.normalHosts.end());
            }
        } else {
            warning() << "node: " << from << " isn't a part of set: " << _set->name
                      << " ismaster: " << replyObj;
        }
        failedHost(from);
        return;
    }

    if (reply.isMaster) {
        if (!receivedIsMasterFromMaster(reply)) {
            log() << "node " << from << " believes it is primary, but its election id of "
                  << reply.electionId << " and config version of " << reply.configVersion
                  << " is older than the most recent election id " << _set->maxElectionId
                  << " and config version of " << _set->configVersion;
            failedHost(from);
            return;
        }
    }

    if (_scan->foundUpMaster) {
        // We only update a Node if a master has confirmed it is in the set.
        _set->updateNodeIfInNodes(reply);
    } else {
        receivedIsMasterBeforeFoundMaster(reply);
        _scan->unconfirmedReplies.push_back(reply);
    }

    // _set->nodes may still not have any nodes with isUp==true, but we have at least found a
    // connectible host that is that claims to be in the set.
    _scan->foundAnyUpNodes = true;

    // TODO consider only notifying if we've updated a node or we've emptied waitingFor.
    _set->cv.notify_all();

    DEV _set->checkInvariants();
}

void Refresher::failedHost(const HostAndPort& host) {
    _scan->waitingFor.erase(host);

    // Failed hosts can't pass criteria, so the only way they'd effect the _refreshUntilMatches
    // loop is if it was the last host we were waitingFor.
    if (_scan->waitingFor.empty())
        _set->cv.notify_all();

    Node* node = _set->findNode(host);
    if (node)
        node->markFailed();
}

ScanStatePtr Refresher::startNewScan(const SetState* set) {
    const ScanStatePtr scan = std::make_shared<ScanState>();

    // The heuristics we use in deciding the order to contact hosts are designed to find a
    // master as quickly as possible. This is because we can't use any hosts we find until
    // we either get the latest set of members from a master or talk to all possible hosts
    // without finding a master.

    // TODO It might make sense to check down nodes first if the last seen master is still
    // marked as up.

    int upNodes = 0;
    for (Nodes::const_iterator it(set->nodes.begin()), end(set->nodes.end()); it != end; ++it) {
        if (it->isUp) {
            // scan the nodes we think are up first
            scan->hostsToScan.push_front(it->host);
            upNodes++;
        } else {
            scan->hostsToScan.push_back(it->host);
        }
    }

    // shuffle the queue, but keep "up" nodes at the front
    std::random_shuffle(scan->hostsToScan.begin(), scan->hostsToScan.begin() + upNodes, set->rand);
    std::random_shuffle(scan->hostsToScan.begin() + upNodes, scan->hostsToScan.end(), set->rand);

    if (!set->lastSeenMaster.empty()) {
        // move lastSeenMaster to front of queue
        std::stable_partition(
            scan->hostsToScan.begin(), scan->hostsToScan.end(), HostIs(set->lastSeenMaster));
    }

    return scan;
}

bool Refresher::receivedIsMasterFromMaster(const IsMasterReply& reply) {
    invariant(reply.isMaster);

    // Reject if config version is older. This is for backwards compatibility with nodes in pv0
    // since they don't have the same ordering with pv1 electionId.
    if (reply.configVersion < _set->configVersion) {
        return false;
    }

    if (reply.electionId.isSet()) {
        // ElectionIds are only comparable if they are of the same protocol version. However, since
        // isMaster has no protocol version field, we use the configVersion instead. This works
        // because configVersion needs to be incremented whenever the protocol version is changed.
        if (reply.configVersion == _set->configVersion && _set->maxElectionId.isSet() &&
            _set->maxElectionId.compare(reply.electionId) > 0) {
            return false;
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
    // REMINDER: both _set->nodes and reply.normalHosts are sorted.
    if (_set->nodes.size() != reply.normalHosts.size() ||
        !std::equal(
            _set->nodes.begin(), _set->nodes.end(), reply.normalHosts.begin(), hostsEqual)) {
        LOG(2) << "Adjusting nodes in our view of replica set " << _set->name
               << " based on master reply: " << reply.raw;

        // remove non-members from _set->nodes
        _set->nodes.erase(
            std::remove_if(_set->nodes.begin(), _set->nodes.end(), HostNotIn(reply.normalHosts)),
            _set->nodes.end());

        // add new members to _set->nodes
        for (std::set<HostAndPort>::const_iterator it = reply.normalHosts.begin();
             it != reply.normalHosts.end();
             ++it) {
            _set->findOrCreateNode(*it);
        }

        // replace hostToScan queue with untried normal hosts. can both add and remove
        // hosts from the queue.
        _scan->hostsToScan.clear();
        _scan->enqueAllUntriedHosts(reply.normalHosts, _set->rand);

        if (!_scan->waitingFor.empty()) {
            // make sure we don't wait for any hosts that aren't considered members
            std::set<HostAndPort> newWaitingFor;
            std::set_intersection(reply.normalHosts.begin(),
                                  reply.normalHosts.end(),
                                  _scan->waitingFor.begin(),
                                  _scan->waitingFor.end(),
                                  std::inserter(newWaitingFor, newWaitingFor.end()));
            _scan->waitingFor.swap(newWaitingFor);
        }
    }

    if (reply.normalHosts != _set->seedNodes) {
        const string oldAddr = _set->getConfirmedServerAddress();
        _set->seedNodes = reply.normalHosts;

        // LogLevel can be pretty low, since replica set reconfiguration should be pretty rare
        // and we want to record our changes
        log() << "changing hosts to " << _set->getConfirmedServerAddress() << " from " << oldAddr;

        if (syncConfigChangeHook) {
            syncConfigChangeHook(_set->name, _set->getConfirmedServerAddress());
        }

        if (asyncConfigChangeHook) {
            // call from a separate thread to avoid blocking and holding lock while potentially
            // going over the network
            stdx::thread bg(asyncConfigChangeHook, _set->name, _set->getConfirmedServerAddress());
            bg.detach();
        }
    }

    // Update other nodes's information based on replies we've already seen
    for (UnconfirmedReplies::iterator it = _scan->unconfirmedReplies.begin();
         it != _scan->unconfirmedReplies.end();
         ++it) {
        // this ignores replies from hosts not in _set->nodes (as modified above)
        _set->updateNodeIfInNodes(*it);
    }
    _scan->unconfirmedReplies.clear();

    _scan->foundUpMaster = true;
    _set->lastSeenMaster = reply.host;

    return true;
}

void Refresher::receivedIsMasterBeforeFoundMaster(const IsMasterReply& reply) {
    invariant(!reply.isMaster);
    // This function doesn't alter _set at all. It only modifies the work queue in _scan.

    // Add everyone this host claims is in the set to possibleNodes.
    _scan->possibleNodes.insert(reply.normalHosts.begin(), reply.normalHosts.end());

    // If this node thinks the primary is someone we haven't tried, make that the next
    // hostToScan.
    if (!reply.primary.empty() && !_scan->triedHosts.count(reply.primary)) {
        std::deque<HostAndPort>::iterator it = std::stable_partition(
            _scan->hostsToScan.begin(), _scan->hostsToScan.end(), HostIs(reply.primary));

        if (it == _scan->hostsToScan.begin()) {
            // reply.primary wasn't in hostsToScan
            _scan->hostsToScan.push_front(reply.primary);
        }
    }
}

HostAndPort Refresher::_refreshUntilMatches(const ReadPreferenceSetting* criteria) {
    stdx::unique_lock<stdx::mutex> lk(_set->mutex);
    while (true) {
        if (criteria) {
            HostAndPort out = _set->getMatchingHost(*criteria);
            if (!out.empty())
                return out;
        }

        const NextStep ns = getNextStep();
        DEV _set->checkInvariants();

        switch (ns.step) {
            case NextStep::DONE:
                // getNextStep may have updated nodes if no master was found
                return criteria ? _set->getMatchingHost(*criteria) : HostAndPort();

            case NextStep::WAIT:  // TODO consider treating as DONE for refreshAll
                _set->cv.wait(lk);
                continue;

            case NextStep::CONTACT_HOST: {
                BSONObj reply;  // empty on error
                int64_t pingMicros = 0;

                lk.unlock();  // relocked after attempting to call isMaster
                try {
                    ScopedDbConnection conn(ConnectionString(ns.host), socketTimeoutSecs);
                    bool ignoredOutParam = false;
                    Timer timer;
                    conn->isMaster(ignoredOutParam, &reply);
                    pingMicros = timer.micros();
                    conn.done();  // return to pool on success.
                } catch (...) {
                    reply = BSONObj();  // should be a no-op but want to be sure
                }
                lk.lock();

                // Ignore the reply and return if we are no longer the current scan. This might
                // happen if it was decided that the host we were contacting isn't part of the set.
                if (_scan != _set->currentScan)
                    return criteria ? _set->getMatchingHost(*criteria) : HostAndPort();

                if (reply.isEmpty())
                    failedHost(ns.host);
                else
                    receivedIsMaster(ns.host, pingMicros, reply);
            }
        }
    }
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
        normalHosts.clear();
        BSONForEach(host, raw.getObjectField("hosts")) {
            normalHosts.insert(HostAndPort(host.String()));
        }
        BSONForEach(host, raw.getObjectField("passives")) {
            normalHosts.insert(HostAndPort(host.String()));
        }

        tags = raw.getObjectField("tags");
    } catch (const std::exception& e) {
        ok = false;
        log() << "exception while parsing isMaster reply: " << e.what() << " " << obj;
    }
}

Node::Node(const HostAndPort& host) : host(host), latencyMicros(unknownLatency) {}

void Node::markFailed() {
    LOG(1) << "Marking host " << host << " as failed";

    isUp = false;
    isMaster = false;
}

bool Node::matches(const ReadPreference pref) const {
    if (!isUp)
        return false;

    if (pref == ReadPreference::PrimaryOnly) {
        return isMaster;
    }

    if (pref == ReadPreference::SecondaryOnly) {
        if (isMaster)
            return false;
    }

    return true;
}

bool Node::matches(const BSONObj& tag) const {
    BSONForEach(tagCriteria, tag) {
        if (this->tags[tagCriteria.fieldNameStringData()] != tagCriteria)
            return false;
    }

    return true;
}

void Node::update(const IsMasterReply& reply) {
    invariant(host == reply.host);
    invariant(reply.ok);

    LOG(3) << "Updating host " << host << " based on ismaster reply: " << reply.raw;

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
}

SetState::SetState(StringData name, const std::set<HostAndPort>& seedNodes)
    : name(name.toString()),
      consecutiveFailedScans(0),
      seedNodes(seedNodes),
      latencyThresholdMicros(serverGlobalParams.defaultLocalThresholdMillis * 1000),
      rand(int64_t(time(0))),
      roundRobin(0) {
    uassert(13642, "Replica set seed list can't be empty", !seedNodes.empty());

    if (name.empty())
        warning() << "Replica set name empty, first node: " << *(seedNodes.begin());

    // This adds the seed hosts to nodes, but they aren't usable for anything except seeding a
    // scan until we start a scan and either find a master or contact all hosts without finding
    // one.
    // WARNING: if seedNodes is ever changed to not imply sorted iteration, you will need to
    // sort nodes after this loop.
    for (std::set<HostAndPort>::const_iterator it = seedNodes.begin(); it != seedNodes.end();
         ++it) {
        nodes.push_back(Node(*it));
    }

    DEV checkInvariants();
}

bool SetState::isUsable() const {
    return consecutiveFailedScans < maxConsecutiveFailedChecks;
}

HostAndPort SetState::getMatchingHost(const ReadPreferenceSetting& criteria) const {
    switch (criteria.pref) {
        // "Prefered" read preferences are defined in terms of other preferences
        case ReadPreference::PrimaryPreferred: {
            HostAndPort out =
                getMatchingHost(ReadPreferenceSetting(ReadPreference::PrimaryOnly, criteria.tags));
            // NOTE: the spec says we should use the primary even if tags don't match
            if (!out.empty())
                return out;
            return getMatchingHost(
                ReadPreferenceSetting(ReadPreference::SecondaryOnly, criteria.tags));
        }

        case ReadPreference::SecondaryPreferred: {
            HostAndPort out = getMatchingHost(
                ReadPreferenceSetting(ReadPreference::SecondaryOnly, criteria.tags));
            if (!out.empty())
                return out;
            // NOTE: the spec says we should use the primary even if tags don't match
            return getMatchingHost(
                ReadPreferenceSetting(ReadPreference::PrimaryOnly, criteria.tags));
        }

        case ReadPreference::PrimaryOnly: {
            // NOTE: isMaster implies isUp
            Nodes::const_iterator it = std::find_if(nodes.begin(), nodes.end(), isMaster);
            if (it == nodes.end())
                return HostAndPort();
            return it->host;
        }

        // The difference between these is handled by Node::matches
        case ReadPreference::SecondaryOnly:
        case ReadPreference::Nearest: {
            BSONForEach(tagElem, criteria.tags.getTagBSON()) {
                uassert(16358, "Tags should be a BSON object", tagElem.isABSONObj());
                BSONObj tag = tagElem.Obj();

                std::vector<const Node*> matchingNodes;
                for (size_t i = 0; i < nodes.size(); i++) {
                    if (nodes[i].matches(criteria.pref) && nodes[i].matches(tag)) {
                        matchingNodes.push_back(&nodes[i]);
                    }
                }

                // don't do more complicated selection if not needed
                if (matchingNodes.empty())
                    continue;
                if (matchingNodes.size() == 1)
                    return matchingNodes.front()->host;

                // order by latency and don't consider hosts further than a threshold from the
                // closest.
                std::sort(matchingNodes.begin(), matchingNodes.end(), compareLatencies);
                for (size_t i = 1; i < matchingNodes.size(); i++) {
                    int64_t distance =
                        matchingNodes[i]->latencyMicros - matchingNodes[0]->latencyMicros;
                    if (distance >= latencyThresholdMicros) {
                        // this node and all remaining ones are too far away
                        matchingNodes.erase(matchingNodes.begin() + i, matchingNodes.end());
                        break;
                    }
                }

                // of the remaining nodes, pick one at random (or use round-robin)
                if (ReplicaSetMonitor::useDeterministicHostSelection) {
                    // only in tests
                    return matchingNodes[roundRobin++ % matchingNodes.size()]->host;
                } else {
                    // normal case
                    return matchingNodes[rand.nextInt32(matchingNodes.size())]->host;
                };
            }

            return HostAndPort();
        }

        default:
            uassert(16337, "Unknown read preference", false);
            break;
    }
}

Node* SetState::findNode(const HostAndPort& host) {
    const Nodes::iterator it = std::lower_bound(nodes.begin(), nodes.end(), host, compareHosts);
    if (it == nodes.end() || it->host != host)
        return NULL;

    return &(*it);
}

Node* SetState::findOrCreateNode(const HostAndPort& host) {
    // This is insertion sort, but N is currently guaranteed to be <= 12 (although this class
    // must function correctly even with more nodes). If we lift that restriction, we may need
    // to consider alternate algorithms.
    Nodes::iterator it = std::lower_bound(nodes.begin(), nodes.end(), host, compareHosts);
    if (it == nodes.end() || it->host != host) {
        LOG(2) << "Adding node " << host << " to our view of replica set " << name;
        it = nodes.insert(it, Node(host));
    }
    return &(*it);
}

void SetState::updateNodeIfInNodes(const IsMasterReply& reply) {
    Node* node = findNode(reply.host);
    if (!node) {
        LOG(2) << "Skipping application of ismaster reply from " << reply.host
               << " since it isn't a confirmed member of set " << name;
        return;
    }

    node->update(reply);
}

std::string SetState::getConfirmedServerAddress() const {
    StringBuilder ss;
    if (!name.empty())
        ss << name << "/";

    for (std::set<HostAndPort>::const_iterator it = seedNodes.begin(); it != seedNodes.end();
         ++it) {
        if (it != seedNodes.begin())
            ss << ",";
        it->append(ss);
    }

    return ss.str();
}

std::string SetState::getUnconfirmedServerAddress() const {
    StringBuilder ss;
    if (!name.empty())
        ss << name << "/";

    for (std::vector<Node>::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (it != nodes.begin())
            ss << ",";
        it->host.append(ss);
    }

    return ss.str();
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
            invariant(nodes[i].host == lastSeenMaster);
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
    std::random_shuffle(hostsToScan.begin(), hostsToScan.end(), rand);
}
}
