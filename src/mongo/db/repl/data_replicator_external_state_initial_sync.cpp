// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
