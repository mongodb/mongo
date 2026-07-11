// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

class OperationContext;
class Timestamp;

namespace rpc {
class ReplSetMetadata;
class OplogQueryMetadata;
}  // namespace rpc

namespace repl {

class OpTime;
struct SyncSourceResolverResponse;

enum class ChangeSyncSourceAction {
    kContinueSyncing,
    kStopSyncingAndDropLastBatchIfPresent,
    kStopSyncingAndEnqueueLastBatch
};

inline std::ostream& operator<<(std::ostream& os, const ChangeSyncSourceAction action) {
    switch (action) {
        case ChangeSyncSourceAction::kContinueSyncing:
            os << "kContinueSyncing";
            break;
        case ChangeSyncSourceAction::kStopSyncingAndDropLastBatchIfPresent:
            os << "kStopSyncingAndDropLastBatchIfPresent";
            break;
        case ChangeSyncSourceAction::kStopSyncingAndEnqueueLastBatch:
            os << "kStopSyncingAndEnqueueLastBatch";
            break;
    }

    return os;
}

/**
 * Manage list of viable and blocked sync sources that we can replicate from.
 */
class [[MONGO_MOD_OPEN]] SyncSourceSelector {
    SyncSourceSelector(const SyncSourceSelector&) = delete;
    SyncSourceSelector& operator=(const SyncSourceSelector&) = delete;

public:
    SyncSourceSelector() = default;
    virtual ~SyncSourceSelector() = default;

    /**
     * Clears the list of sync sources we have denylisted.
     */
    virtual void clearSyncSourceDenylist() = 0;

    /**
     * Chooses a viable sync source, or, if none available, returns empty HostAndPort.
     */
    virtual HostAndPort chooseNewSyncSource(const OpTime& lastOpTimeFetched) = 0;

    /**
     * Denylists choosing 'host' as a sync source until time 'until'.
     */
    virtual void denylistSyncSource(const HostAndPort& host, Date_t until) = 0;

    /**
     * Determines if a new sync source should be chosen, if a better candidate sync source is
     * available.  If the current sync source's last optime (visibleOpTime or appliedOpTime of
     * metadata under protocolVersion 1, but pulled from the MemberData in protocolVersion 0)
     * is more than _maxSyncSourceLagSecs behind any syncable source, this function returns true.
     * If we are running in ProtocolVersion 1, our current sync source is not primary, has no sync
     * source and only has data up to "myLastOpTime", returns true.
     *
     * "now" is used to skip over currently denylisted sync sources.
     */
    virtual ChangeSyncSourceAction shouldChangeSyncSource(
        const HostAndPort& currentSource,
        const rpc::ReplSetMetadata& replMetadata,
        const rpc::OplogQueryMetadata& oqMetadata,
        const OpTime& previousOpTimeFetched,
        const OpTime& lastOpTimeFetched) const = 0;

    /*
     * Determines if a new sync source should be chosen when an error occures during fetching,
     * without attempting retries on the same sync source.
     * Because metadata is not available, checks are a subset of those in shouldChangeSyncSource.
     */
    virtual ChangeSyncSourceAction shouldChangeSyncSourceOnError(
        const HostAndPort& currentSource, const OpTime& lastOpTimeFetched) const = 0;
};

}  // namespace repl
}  // namespace mongo
