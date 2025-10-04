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

#include "mongo/db/repl/data_replicator_external_state_initial_sync.h"

namespace mongo {
namespace repl {

DataReplicatorExternalStateInitialSync::DataReplicatorExternalStateInitialSync(
    ReplicationCoordinator* replicationCoordinator,
    ReplicationCoordinatorExternalState* replicationCoordinatorExternalState)
    : DataReplicatorExternalStateImpl(replicationCoordinator, replicationCoordinatorExternalState) {
}

ChangeSyncSourceAction DataReplicatorExternalStateInitialSync::shouldStopFetching(
    const HostAndPort&,
    const rpc::ReplSetMetadata&,
    const rpc::OplogQueryMetadata&,
    const OpTime& previousOpTimeFetched,
    const OpTime& lastOpTimeFetched) const {

    // Since initial sync does not allow for sync source changes, it should not check if there are
    // better sync sources. If there is a problem on the sync source, it will manifest itself in the
    // cloning phase as well, and cause a failure there.

    return ChangeSyncSourceAction::kContinueSyncing;
}

ChangeSyncSourceAction DataReplicatorExternalStateInitialSync::shouldStopFetchingOnError(
    const HostAndPort&, const OpTime& lastOpTimeFetched) const {

    return ChangeSyncSourceAction::kContinueSyncing;
}

}  // namespace repl
}  // namespace mongo
