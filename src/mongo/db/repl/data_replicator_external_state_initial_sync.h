// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/db/repl/data_replicator_external_state_impl.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {

/**
 * Data replicator external state implementation for initial sync.
 */

class DataReplicatorExternalStateInitialSync : public DataReplicatorExternalStateImpl {
public:
    DataReplicatorExternalStateInitialSync(
        ReplicationCoordinator* replicationCoordinator,
        ReplicationCoordinatorExternalState* replicationCoordinatorExternalState);

    ChangeSyncSourceAction shouldStopFetching(const HostAndPort& source,
                                              const rpc::ReplSetMetadata& replMetadata,
                                              const rpc::OplogQueryMetadata& oqMetadata,
                                              const OpTime& previousOpTimeFetched,
                                              const OpTime& lastOpTimeFetched) const override;

    ChangeSyncSourceAction shouldStopFetchingOnError(
        const HostAndPort& source, const OpTime& lastOpTimeFetched) const override;
};

}  // namespace repl
}  // namespace mongo
