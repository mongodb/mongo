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

#include "mongo/db/repl/replication_coordinator_mock.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/hello/hello_response.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/session/internal_session_pool.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/future_impl.h"

#include <mutex>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace repl {

namespace {

/**
 * Helper to create default ReplSettings for tests that represents a one-node replica set.
 */
ReplSettings createReplSettingsForSingleNodeReplSet() {
    ReplSettings settings;
    settings.setOplogSizeBytes(5 * 1024 * 1024);
    settings.setReplSetString("mySet/node1:12345");
    return settings;
}

}  // namespace

ReplicationCoordinatorMock::ReplicationCoordinatorMock(ServiceContext* service,
                                                       const ReplSettings& settings)
    : _service(service),
      _settings(settings),
      _splitSessionManager(InternalSessionPool::get(service)) {}

ReplicationCoordinatorMock::ReplicationCoordinatorMock(ServiceContext* service,
                                                       StorageInterface* storage)
    : ReplicationCoordinatorMock(service, createReplSettingsForSingleNodeReplSet()) {
    _storage = storage;
}

ReplicationCoordinatorMock::ReplicationCoordinatorMock(ServiceContext* service)
    : ReplicationCoordinatorMock(service, createReplSettingsForSingleNodeReplSet()) {}

ReplicationCoordinatorMock::~ReplicationCoordinatorMock() {}

void ReplicationCoordinatorMock::startup(OperationContext* opCtx,
                                         StorageEngine::LastShutdownState lastShutdownState) {
    // TODO
}

void ReplicationCoordinatorMock::enterTerminalShutdown() {
    // TODO
}

bool ReplicationCoordinatorMock::enterQuiesceModeIfSecondary(Milliseconds quiesceTime) {
    // TODO
    return true;
}

bool ReplicationCoordinatorMock::inQuiesceMode() const {
    // TODO
    return false;
}

void ReplicationCoordinatorMock::shutdown(OperationContext*,
                                          BSONObjBuilder* shutdownTimeElapsedBuilder) {
    // TODO
}

const ReplSettings& ReplicationCoordinatorMock::getSettings() const {
    return _settings;
}

MemberState ReplicationCoordinatorMock::getMemberState() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    return _memberState;
}

std::vector<MemberData> ReplicationCoordinatorMock::getMemberData() const {
    MONGO_UNREACHABLE;
    return {};
}

bool ReplicationCoordinatorMock::canAcceptNonLocalWrites() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    return _canAcceptNonLocalWrites;
}

void ReplicationCoordinatorMock::setCanAcceptNonLocalWrites(bool canAcceptNonLocalWrites) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    _canAcceptNonLocalWrites = canAcceptNonLocalWrites;
}

Status ReplicationCoordinatorMock::waitForMemberState(Interruptible* interruptible,
                                                      MemberState expectedState,
                                                      Milliseconds timeout) {
    MONGO_UNREACHABLE;
    return Status::OK();
}

bool ReplicationCoordinatorMock::isInPrimaryOrSecondaryState(OperationContext* opCtx) const {
    return isInPrimaryOrSecondaryState_UNSAFE();
}

bool ReplicationCoordinatorMock::isInPrimaryOrSecondaryState_UNSAFE() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    return _memberState.primary() || _memberState.secondary();
}

Seconds ReplicationCoordinatorMock::getSecondaryDelaySecs() const {
    return _secondaryDelaySecs;
}

void ReplicationCoordinatorMock::setSecondaryDelaySecs(Seconds sec) {
    _secondaryDelaySecs = sec;
}

void ReplicationCoordinatorMock::clearSyncSourceDenylist() {}

ReplicationCoordinator::StatusAndDuration ReplicationCoordinatorMock::awaitReplication(
    OperationContext* opCtx, const OpTime& opTime, const WriteConcernOptions& writeConcern) {
    return _awaitReplicationReturnValueFunction(opCtx, opTime);
}

void ReplicationCoordinatorMock::setAwaitReplicationReturnValueFunction(
    AwaitReplicationReturnValueFunction returnValueFunction) {
    _awaitReplicationReturnValueFunction = std::move(returnValueFunction);
}

SharedSemiFuture<void> ReplicationCoordinatorMock::awaitReplicationAsyncNoWTimeout(
    const OpTime& opTime, const WriteConcernOptions& writeConcern) {
    auto opCtx = cc().makeOperationContext();
    auto result = _awaitReplicationReturnValueFunction(opCtx.get(), opTime);
    return Future<ReplicationCoordinator::StatusAndDuration>::makeReady(result).ignoreValue();
}

void ReplicationCoordinatorMock::stepDown(OperationContext* opCtx,
                                          bool force,
                                          const Milliseconds& waitTime,
                                          const Milliseconds& stepdownTime) {}

bool ReplicationCoordinatorMock::isWritablePrimaryForReportingPurposes() {
    // TODO
    return true;
}

bool ReplicationCoordinatorMock::canAcceptWritesForDatabase(OperationContext* opCtx,
                                                            const DatabaseName& dbName) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // Return true if we allow writes explicitly even when not in primary state, as in sharding
    // unit tests, so that the op observers can fire but the tests don't have to set all the states
    // as if it's in primary.
    if (_alwaysAllowWrites) {
        return true;
    }
    return dbName == DatabaseName::kLocal || _memberState.primary();
}

bool ReplicationCoordinatorMock::canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx,
                                                                   const DatabaseName& dbName) {
    return canAcceptWritesForDatabase(opCtx, dbName);
}

bool ReplicationCoordinatorMock::canAcceptWritesFor(OperationContext* opCtx,
                                                    const NamespaceStringOrUUID& nsOrUUID) {
    // TODO
    return canAcceptWritesForDatabase(opCtx, nsOrUUID.dbName());
}

bool ReplicationCoordinatorMock::canAcceptWritesFor_UNSAFE(OperationContext* opCtx,
                                                           const NamespaceStringOrUUID& nsOrUUID) {
    return canAcceptWritesFor(opCtx, nsOrUUID);
}

Status ReplicationCoordinatorMock::checkCanServeReadsFor(OperationContext* opCtx,
                                                         const NamespaceString& ns,
                                                         bool secondaryOk) {
    // TODO
    return Status::OK();
}

Status ReplicationCoordinatorMock::checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                                                const NamespaceString& ns,
                                                                bool secondaryOk) {
    return checkCanServeReadsFor(opCtx, ns, secondaryOk);
}

bool ReplicationCoordinatorMock::shouldRelaxIndexConstraints(OperationContext* opCtx,
                                                             const NamespaceString& ns) {
    return (!canAcceptWritesFor(opCtx, ns));
}

void ReplicationCoordinatorMock::setMyHeartbeatMessage(const std::string& msg) {
    // TODO
}

void ReplicationCoordinatorMock::_setMyLastAppliedOpTimeAndWallTime(
    WithLock lk, const OpTimeAndWallTime& opTimeAndWallTime) {
    _myLastAppliedOpTime = opTimeAndWallTime.opTime;
    _myLastAppliedWallTime = opTimeAndWallTime.wallTime;

    if (!_updateCommittedSnapshot) {
        return;
    }

    if (auto storageEngine = _service->getStorageEngine()) {
        // Use the "all durable" timestamp for the committed snapshot rather than the one provided.
        // This ensures that we never set the committed snapshot to a timestamp that contains oplog
        // holes.
        auto allDurable = storageEngine->getAllDurableTimestamp();
        _setCurrentCommittedSnapshotOpTime(lk, {allDurable, opTimeAndWallTime.opTime.getTerm()});
        if (auto snapshotManager = storageEngine->getSnapshotManager()) {
            snapshotManager->setCommittedSnapshot(allDurable);
        }
    } else {
        _setCurrentCommittedSnapshotOpTime(lk, opTimeAndWallTime.opTime);
    }
}

void ReplicationCoordinatorMock::setMyLastWrittenOpTimeAndWallTimeForward(
    const OpTimeAndWallTime& opTimeAndWallTime) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // = is necessary here because in some unit test setup, we want to update the term while not
    // changing the opTime.
    if (opTimeAndWallTime.opTime >= _myLastWrittenOpTime) {
        _myLastWrittenOpTime = opTimeAndWallTime.opTime;
        _myLastWrittenWallTime = opTimeAndWallTime.wallTime;
    }
}

void ReplicationCoordinatorMock::setMyLastAppliedOpTimeAndWallTimeForward(
    const OpTimeAndWallTime& opTimeAndWallTime) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // = is necessary here because in some unit test setup, we want to update the term while not
    // changing the opTime.
    if (opTimeAndWallTime.opTime >= _myLastAppliedOpTime) {
        _setMyLastAppliedOpTimeAndWallTime(lk, opTimeAndWallTime);
    }
}

void ReplicationCoordinatorMock::setMyLastDurableOpTimeAndWallTimeForward(
    const OpTimeAndWallTime& opTimeAndWallTime) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // = is necessary here because in some unit test setup, we want to update the term while not
    // changing the opTime.
    if (opTimeAndWallTime.opTime >= _myLastDurableOpTime) {
        _myLastDurableOpTime = opTimeAndWallTime.opTime;
        _myLastDurableWallTime = opTimeAndWallTime.wallTime;
    }
}

void ReplicationCoordinatorMock::setMyLastAppliedAndLastWrittenOpTimeAndWallTimeForward(
    const OpTimeAndWallTime& opTimeAndWallTime) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // = is necessary here because in some unit test setup, we want to update the term while not
    // changing the opTime.
    if (opTimeAndWallTime.opTime >= _myLastWrittenOpTime) {
        _myLastWrittenOpTime = opTimeAndWallTime.opTime;
        _myLastWrittenWallTime = opTimeAndWallTime.wallTime;
    }

    if (opTimeAndWallTime.opTime >= _myLastAppliedOpTime) {
        _setMyLastAppliedOpTimeAndWallTime(lk, opTimeAndWallTime);
    }
}

void ReplicationCoordinatorMock::setMyLastDurableAndLastWrittenOpTimeAndWallTimeForward(
    const OpTimeAndWallTime& opTimeAndWallTime) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // = is necessary here because in some unit test setup, we want to update the term while not
    // changing the opTime.
    if (opTimeAndWallTime.opTime >= _myLastWrittenOpTime) {
        _myLastWrittenOpTime = opTimeAndWallTime.opTime;
        _myLastWrittenWallTime = opTimeAndWallTime.wallTime;
    }

    if (opTimeAndWallTime.opTime >= _myLastDurableOpTime) {
        _myLastDurableOpTime = opTimeAndWallTime.opTime;
        _myLastDurableWallTime = opTimeAndWallTime.wallTime;
    }
}

void ReplicationCoordinatorMock::resetMyLastOpTimes() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    _myLastDurableOpTime = OpTime();
    _myLastDurableWallTime = Date_t();
}

OpTimeAndWallTime ReplicationCoordinatorMock::getMyLastWrittenOpTimeAndWallTime(
    bool rollbackSafe) const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (rollbackSafe && _memberState.rollback()) {
        return {};
    }
    return {_myLastWrittenOpTime, _myLastWrittenWallTime};
}

OpTime ReplicationCoordinatorMock::getMyLastWrittenOpTime() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    return _myLastWrittenOpTime;
}

OpTimeAndWallTime ReplicationCoordinatorMock::getMyLastAppliedOpTimeAndWallTime() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return {_myLastAppliedOpTime, _myLastAppliedWallTime};
}

OpTime ReplicationCoordinatorMock::getMyLastAppliedOpTime() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    return _myLastAppliedOpTime;
}

OpTimeAndWallTime ReplicationCoordinatorMock::getMyLastDurableOpTimeAndWallTime() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    return {_myLastDurableOpTime, _myLastDurableWallTime};
}

OpTime ReplicationCoordinatorMock::getMyLastDurableOpTime() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    return _myLastDurableOpTime;
}

Status ReplicationCoordinatorMock::waitUntilMajorityOpTime(mongo::OperationContext* opCtx,
                                                           mongo::repl::OpTime targetOpTime,
                                                           boost::optional<Date_t> deadline) {
    return Status::OK();
}

Status ReplicationCoordinatorMock::waitUntilOpTimeForRead(OperationContext* opCtx,
                                                          const ReadConcernArgs& settings) {
    return Status::OK();
}

Status ReplicationCoordinatorMock::waitUntilOpTimeForReadUntil(OperationContext* opCtx,
                                                               const ReadConcernArgs& settings,
                                                               boost::optional<Date_t> deadline) {
    return Status::OK();
}

Status ReplicationCoordinatorMock::waitUntilOpTimeWrittenUntil(OperationContext* opCtx,
                                                               LogicalTime clusterTime,
                                                               boost::optional<Date_t> deadline) {
    return Status::OK();
}

Status ReplicationCoordinatorMock::awaitTimestampCommitted(OperationContext* opCtx, Timestamp ts) {
    return Status::OK();
}

OID ReplicationCoordinatorMock::getElectionId() {
    // TODO
    return OID();
}

int ReplicationCoordinatorMock::getMyId() const {
    return 0;
}

HostAndPort ReplicationCoordinatorMock::getMyHostAndPort() const {
    return HostAndPort();
}

Status ReplicationCoordinatorMock::setFollowerMode(const MemberState& newState) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    _memberState = newState;
    return Status::OK();
}

Status ReplicationCoordinatorMock::setFollowerModeRollback(OperationContext* opCtx) {
    return setFollowerMode(MemberState::RS_ROLLBACK);
}

void ReplicationCoordinatorMock::setOplogSyncState(const OplogSyncState& newState) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _oplogSyncState = newState;
}

ReplicationCoordinator::OplogSyncState ReplicationCoordinatorMock::getOplogSyncState() {
    return _oplogSyncState;
}

void ReplicationCoordinatorMock::signalWriterDrainComplete(OperationContext*, long long) noexcept {}

void ReplicationCoordinatorMock::signalApplierDrainComplete(OperationContext*, long long) noexcept {
}

void ReplicationCoordinatorMock::signalUpstreamUpdater() {}

StatusWith<BSONObj> ReplicationCoordinatorMock::prepareReplSetUpdatePositionCommand() const {
    BSONObjBuilder cmdBuilder;
    cmdBuilder.append("replSetUpdatePosition", 1);
    return cmdBuilder.obj();
}

ReplSetConfig ReplicationCoordinatorMock::getConfig() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    return _getConfigReturnValue;
}

ConnectionString ReplicationCoordinatorMock::getConfigConnectionString() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _getConfigReturnValue.getConnectionString();
}

std::int64_t ReplicationCoordinatorMock::getConfigTerm() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _getConfigReturnValue.getConfigTerm();
}

std::int64_t ReplicationCoordinatorMock::getConfigVersion() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _getConfigReturnValue.getConfigVersion();
}

ConfigVersionAndTerm ReplicationCoordinatorMock::getConfigVersionAndTerm() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _getConfigReturnValue.getConfigVersionAndTerm();
}

Status ReplicationCoordinatorMock::validateWriteConcern(
    const WriteConcernOptions& writeConcern) const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _getConfigReturnValue.validateWriteConcern(writeConcern);
}

boost::optional<MemberConfig> ReplicationCoordinatorMock::findConfigMemberByHostAndPort_deprecated(
    const HostAndPort& hap) const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    const MemberConfig* result = _getConfigReturnValue.findMemberByHostAndPort(hap);
    return boost::make_optional(result, *result);
}

void ReplicationCoordinatorMock::setGetConfigReturnValue(ReplSetConfig returnValue) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    _getConfigReturnValue = std::move(returnValue);
}

void ReplicationCoordinatorMock::processReplSetGetConfig(BSONObjBuilder* result,
                                                         bool commitmentStatus,
                                                         bool includeNewlyAdded) {
    // TODO
}

void ReplicationCoordinatorMock::processReplSetMetadata(const rpc::ReplSetMetadata& replMetadata) {}

void ReplicationCoordinatorMock::advanceCommitPoint(
    const OpTimeAndWallTime& committedOptimeAndWallTime, bool fromSyncSource) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _lastCommittedOpTime = committedOptimeAndWallTime.opTime;
    _lastCommittedWallTime = committedOptimeAndWallTime.wallTime;
}

void ReplicationCoordinatorMock::cancelAndRescheduleElectionTimeout() {}

Status ReplicationCoordinatorMock::processReplSetGetStatus(OperationContext* opCtx,
                                                           BSONObjBuilder*,
                                                           ReplSetGetStatusResponseStyle) {
    return Status::OK();
}

void ReplicationCoordinatorMock::appendSecondaryInfoData(BSONObjBuilder* result) {}

void ReplicationCoordinatorMock::appendConnectionStats(executor::ConnectionPoolStats* stats) const {
}

Status ReplicationCoordinatorMock::setMaintenanceMode(OperationContext* opCtx, bool activate) {
    return Status::OK();
}

bool ReplicationCoordinatorMock::getMaintenanceMode() {
    return false;
}

Status ReplicationCoordinatorMock::processReplSetSyncFrom(OperationContext* opCtx,
                                                          const HostAndPort& target,
                                                          BSONObjBuilder* resultObj) {
    // TODO
    return Status::OK();
}

Status ReplicationCoordinatorMock::processReplSetFreeze(int secs, BSONObjBuilder* resultObj) {
    // TODO
    return Status::OK();
}

Status ReplicationCoordinatorMock::processReplSetReconfig(OperationContext* opCtx,
                                                          const ReplSetReconfigArgs& args,
                                                          BSONObjBuilder* resultObj) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _latestReconfig = args.newConfigObj;
    return Status::OK();
}

Status ReplicationCoordinatorMock::doReplSetReconfig(OperationContext* opCtx,
                                                     GetNewConfigFn getNewConfig,
                                                     bool force) {
    return Status::OK();
}

Status ReplicationCoordinatorMock::doOptimizedReconfig(OperationContext* opCtx,
                                                       GetNewConfigFn getNewConfig) {
    return Status::OK();
}

Status ReplicationCoordinatorMock::awaitConfigCommitment(OperationContext* opCtx,
                                                         bool waitForOplogCommitment,
                                                         long long term) {
    return Status::OK();
}

Status ReplicationCoordinatorMock::processReplSetInitiate(OperationContext* opCtx,
                                                          const BSONObj& configObj,
                                                          BSONObjBuilder* resultObj) {
    return Status::OK();
}

Status ReplicationCoordinatorMock::processReplSetUpdatePosition(const UpdatePositionArgs& updates) {
    // TODO
    return Status::OK();
}

bool ReplicationCoordinatorMock::buildsIndexes() {
    // TODO
    return true;
}

std::vector<HostAndPort> ReplicationCoordinatorMock::getHostsWrittenTo(const OpTime& op,
                                                                       bool durablyWritten) {
    return std::vector<HostAndPort>();
}

Status ReplicationCoordinatorMock::checkIfWriteConcernCanBeSatisfied(
    const WriteConcernOptions& writeConcern) const {
    return Status::OK();
}

Status ReplicationCoordinatorMock::checkIfCommitQuorumCanBeSatisfied(
    const CommitQuorumOptions& commitQuorum) const {
    return Status::OK();
}

bool ReplicationCoordinatorMock::isCommitQuorumSatisfied(
    const CommitQuorumOptions& commitQuorum, const std::vector<mongo::HostAndPort>& members) const {
    return true;
}

WriteConcernOptions ReplicationCoordinatorMock::getGetLastErrorDefault() {
    return WriteConcernOptions();
}

Status ReplicationCoordinatorMock::checkReplEnabledForCommand(BSONObjBuilder* result) {
    // TODO
    return Status::OK();
}

HostAndPort ReplicationCoordinatorMock::chooseNewSyncSource(const OpTime& lastOpTimeFetched) {
    return HostAndPort();
}

void ReplicationCoordinatorMock::denylistSyncSource(const HostAndPort& host, Date_t until) {}

void ReplicationCoordinatorMock::resetLastOpTimesFromOplog(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    _resetLastOpTimesCalled = true;
}

bool ReplicationCoordinatorMock::lastOpTimesWereReset() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    return _resetLastOpTimesCalled;
}

ChangeSyncSourceAction ReplicationCoordinatorMock::shouldChangeSyncSource(
    const HostAndPort& currentSource,
    const rpc::ReplSetMetadata& replMetadata,
    const rpc::OplogQueryMetadata& oqMetadata,
    const OpTime& previousOpTimeFetched,
    const OpTime& lastOpTimeFetched) const {
    MONGO_UNREACHABLE;
}

ChangeSyncSourceAction ReplicationCoordinatorMock::shouldChangeSyncSourceOnError(
    const HostAndPort& currentSource, const OpTime& lastOpTimeFetched) const {
    MONGO_UNREACHABLE;
}

OpTime ReplicationCoordinatorMock::getLastCommittedOpTime() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _lastCommittedOpTime;
}

OpTimeAndWallTime ReplicationCoordinatorMock::getLastCommittedOpTimeAndWallTime() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return {_lastCommittedOpTime, _lastCommittedWallTime};
}

Status ReplicationCoordinatorMock::processReplSetRequestVotes(
    OperationContext* opCtx,
    const ReplSetRequestVotesArgs& args,
    ReplSetRequestVotesResponse* response) {
    return Status::OK();
}

void ReplicationCoordinatorMock::prepareReplMetadata(const GenericArguments& genericArgs,
                                                     const OpTime& lastOpTimeFromClient,
                                                     BSONObjBuilder* builder) const {}

Status ReplicationCoordinatorMock::processHeartbeatV1(const ReplSetHeartbeatArgsV1& args,
                                                      ReplSetHeartbeatResponse* response) {
    return Status::OK();
}

void ReplicationCoordinatorMock::setWriteConcernMajorityShouldJournal(bool shouldJournal) {
    _writeConcernMajorityShouldJournal = shouldJournal;
}

bool ReplicationCoordinatorMock::getWriteConcernMajorityShouldJournal() {
    return _writeConcernMajorityShouldJournal;
}

long long ReplicationCoordinatorMock::getTerm() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    return _term;
}

Status ReplicationCoordinatorMock::updateTerm(OperationContext* opCtx, long long term) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    _term = term;
    return Status::OK();
}

void ReplicationCoordinatorMock::clearCommittedSnapshot() {}

void ReplicationCoordinatorMock::_setCurrentCommittedSnapshotOpTime(WithLock lk, OpTime time) {
    _currentCommittedSnapshotOpTime = time;
}

void ReplicationCoordinatorMock::setCurrentCommittedSnapshotOpTime(OpTime time) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _setCurrentCommittedSnapshotOpTime(lk, time);
}

OpTime ReplicationCoordinatorMock::getCurrentCommittedSnapshotOpTime() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _currentCommittedSnapshotOpTime;
}

void ReplicationCoordinatorMock::waitUntilSnapshotCommitted(OperationContext* opCtx,
                                                            const Timestamp& untilSnapshot) {}

void ReplicationCoordinatorMock::createWMajorityWriteAvailabilityDateWaiter(OpTime opTime) {
    return;
}

Status ReplicationCoordinatorMock::waitForPrimaryMajorityReadsAvailable(
    OperationContext* opCtx) const {
    return Status::OK();
}

WriteConcernOptions ReplicationCoordinatorMock::populateUnsetWriteConcernOptionsSyncMode(
    WriteConcernOptions wc) {
    if (wc.syncMode == WriteConcernOptions::SyncMode::UNSET) {
        if (wc.isMajority() && getWriteConcernMajorityShouldJournal()) {
            wc.syncMode = WriteConcernOptions::SyncMode::JOURNAL;
        } else {
            wc.syncMode = WriteConcernOptions::SyncMode::NONE;
        }
    }
    return wc;
}

Status ReplicationCoordinatorMock::stepUpIfEligible(bool skipDryRun) {
    return Status::OK();
}

void ReplicationCoordinatorMock::alwaysAllowWrites(bool allowWrites) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    _alwaysAllowWrites = allowWrites;
}

Status ReplicationCoordinatorMock::abortCatchupIfNeeded(PrimaryCatchUpConclusionReason reason) {
    return Status::OK();
}

void ReplicationCoordinatorMock::incrementNumCatchUpOpsIfCatchingUp(long numOps) {
    return;
}

boost::optional<Timestamp> ReplicationCoordinatorMock::getRecoveryTimestamp() {
    if (_storage) {
        return _storage->getRecoveryTimestamp(getServiceContext());
    }
    return boost::none;
}

bool ReplicationCoordinatorMock::setContainsArbiter() const {
    return false;
}

void ReplicationCoordinatorMock::attemptToAdvanceStableTimestamp() {
    return;
}

void ReplicationCoordinatorMock::finishRecoveryIfEligible(OperationContext* opCtx) {
    return;
}

void ReplicationCoordinatorMock::updateAndLogStateTransitionMetrics(
    const ReplicationCoordinator::OpsKillingStateTransitionEnum stateTransition,
    const size_t numOpsKilled,
    const size_t numOpsRunning) const {
    return;
}

TopologyVersion ReplicationCoordinatorMock::getTopologyVersion() const {
    return TopologyVersion(repl::instanceId, 0);
}

void ReplicationCoordinatorMock::incrementTopologyVersion() {
    return;
}

SharedSemiFuture<std::shared_ptr<const HelloResponse>>
ReplicationCoordinatorMock::getHelloResponseFuture(
    const SplitHorizon::Parameters& horizonParams,
    boost::optional<TopologyVersion> clientTopologyVersion) {
    auto response =
        awaitHelloResponse(nullptr, horizonParams, clientTopologyVersion, Date_t::now());
    return SharedSemiFuture<std::shared_ptr<const HelloResponse>>(
        std::shared_ptr<const HelloResponse>(response));
}

std::shared_ptr<const HelloResponse> ReplicationCoordinatorMock::awaitHelloResponse(
    OperationContext* opCtx,
    const SplitHorizon::Parameters& horizonParams,
    boost::optional<TopologyVersion> clientTopologyVersion,
    boost::optional<Date_t> deadline) {
    auto config = getConfig();
    auto response = std::make_shared<HelloResponse>();
    response->setReplSetVersion(config.getConfigVersion());
    response->setIsWritablePrimary(true);
    response->setIsSecondary(false);
    if (config.getNumMembers() > 0) {
        for (auto i = 0; i < config.getNumMembers(); ++i) {
            auto hnp = config.getMemberAt(i).getHostAndPort();
            response->addHost(hnp);
            if (i == 0) {
                response->setMe(hnp);
                response->setPrimary(hnp);
            }
        }
    } else {
        response->setMe(HostAndPort::parseThrowing("localhost:27017"));
    }

    response->setElectionId(OID::gen());
    response->setTopologyVersion(TopologyVersion(repl::instanceId, 0));
    return response;
}

StatusWith<OpTime> ReplicationCoordinatorMock::getLatestWriteOpTime(
    OperationContext* opCtx) const noexcept {
    OpTime o = getMyLastWrittenOpTime();
    if (o.isNull()) {
        // ErrorCodes::OplogOperationUnsupported allows the status to be transparently upgraded to
        // OK in setLastOpToSystemLastOpTime.
        return StatusWith<OpTime>(ErrorCodes::OplogOperationUnsupported,
                                  "uninitialized lastWritten");
    }
    return o;
}

HostAndPort ReplicationCoordinatorMock::getCurrentPrimaryHostAndPort() const {
    return HostAndPort();
}

void ReplicationCoordinatorMock::cancelCbkHandle(
    executor::TaskExecutor::CallbackHandle activeHandle) {
    MONGO_UNREACHABLE;
}

BSONObj ReplicationCoordinatorMock::runCmdOnPrimaryAndAwaitResponse(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const BSONObj& cmdObj,
    OnRemoteCmdScheduledFn onRemoteCmdScheduled,
    OnRemoteCmdCompleteFn onRemoteCmdComplete) {
    if (_runCmdOnPrimaryAndAwaitResponseFn) {
        return _runCmdOnPrimaryAndAwaitResponseFn(
            opCtx, dbName, cmdObj, onRemoteCmdScheduled, onRemoteCmdComplete);
    }
    return BSON("ok" << 1);
}
void ReplicationCoordinatorMock::restartScheduledHeartbeats_forTest() {
    return;
}

void ReplicationCoordinatorMock::recordIfCWWCIsSetOnConfigServerOnStartup(OperationContext* opCtx) {
    MONGO_UNREACHABLE;
}

ReplicationCoordinatorMock::WriteConcernTagChanges*
ReplicationCoordinatorMock::getWriteConcernTagChanges() {
    MONGO_UNREACHABLE;
}

SplitPrepareSessionManager* ReplicationCoordinatorMock::getSplitPrepareSessionManager() {
    return &_splitSessionManager;
}

boost::optional<UUID> ReplicationCoordinatorMock::getInitialSyncId(OperationContext* opCtx) {
    return uassertStatusOK(UUID::parse("00000000-0000-0000-0000-000000000000"));
}

void ReplicationCoordinatorMock::setConsistentDataAvailable(OperationContext* opCtx,
                                                            bool isDataMajorityCommitted) {
    ReplicaSetAwareServiceRegistry::get(opCtx->getServiceContext())
        .onConsistentDataAvailable(opCtx, isDataMajorityCommitted, getMemberState().rollback());
}

bool ReplicationCoordinatorMock::isDataConsistent() const {
    // Assume data is always consistent in unittests except during initial sync.
    return !getMemberState().startup2();
}

void ReplicationCoordinatorMock::clearSyncSource() {
    MONGO_UNREACHABLE;
}

}  // namespace repl
}  // namespace mongo
