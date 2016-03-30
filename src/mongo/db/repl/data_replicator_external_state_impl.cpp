/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/data_replicator_external_state_impl.h"

#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

DataReplicatorExternalStateImpl::DataReplicatorExternalStateImpl(
    ReplicationCoordinator* replicationCoordinator)
    : _replicationCoordinator(replicationCoordinator) {}

OpTimeWithTerm DataReplicatorExternalStateImpl::getCurrentTermAndLastCommittedOpTime() {
    if (!_replicationCoordinator->isV1ElectionProtocol()) {
        return {OpTime::kUninitializedTerm, OpTime()};
    }
    return {_replicationCoordinator->getTerm(), _replicationCoordinator->getLastCommittedOpTime()};
}

void DataReplicatorExternalStateImpl::processMetadata(const rpc::ReplSetMetadata& metadata) {
    _replicationCoordinator->processReplSetMetadata(metadata);
    if (metadata.getPrimaryIndex() != rpc::ReplSetMetadata::kNoPrimary) {
        _replicationCoordinator->cancelAndRescheduleElectionTimeout();
    }
}

bool DataReplicatorExternalStateImpl::shouldStopFetching(const HostAndPort& source,
                                                         const OpTime& sourceOpTime,
                                                         bool sourceHasSyncSource) {
    // Re-evaluate quality of sync target.
    if (_replicationCoordinator->shouldChangeSyncSource(
            source, sourceOpTime, sourceHasSyncSource)) {
        LOG(1) << "Canceling oplog query because we have to choose a sync source. Current source: "
               << source << ", OpTime " << sourceOpTime
               << ", hasSyncSource:" << sourceHasSyncSource;
        return true;
    }
    return false;
}

ReplicationCoordinator* DataReplicatorExternalStateImpl::getReplicationCoordinator() const {
    return _replicationCoordinator;
}

}  // namespace repl
}  // namespace mongo
