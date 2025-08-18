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

#pragma once

#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace MONGO_MOD_PUB mongo {

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
class MONGO_MOD_OPEN SyncSourceSelector {
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
}  // namespace MONGO_MOD_PUB mongo
