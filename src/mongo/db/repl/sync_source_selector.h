/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

class OperationContext;
class Timestamp;

namespace rpc {
class ReplSetMetadata;
}

namespace repl {

class OpTime;
struct SyncSourceResolverResponse;

/**
 * Manage list of viable and blocked sync sources that we can replicate from.
 */
class SyncSourceSelector {
    MONGO_DISALLOW_COPYING(SyncSourceSelector);

public:
    SyncSourceSelector() = default;
    virtual ~SyncSourceSelector() = default;

    /**
     * Clears the list of sync sources we have blacklisted.
     */
    virtual void clearSyncSourceBlacklist() = 0;

    /**
     * Chooses a viable sync source, or, if none available, returns empty HostAndPort.
     */
    virtual HostAndPort chooseNewSyncSource(const Timestamp& lastTimestampFetched) = 0;

    /**
     * Blacklists choosing 'host' as a sync source until time 'until'.
     */
    virtual void blacklistSyncSource(const HostAndPort& host, Date_t until) = 0;

    /**
     * Determines if a new sync source should be chosen, if a better candidate sync source is
     * available.  If the current sync source's last optime (visibleOpTime of metadata under
     * protocolVersion 1, but pulled from the MemberHeartbeatData in protocolVersion 0) is more than
     * _maxSyncSourceLagSecs behind any syncable source, this function returns true. If we are
     * running in ProtocolVersion 1, our current sync source is not primary, has no sync source
     * and only has data up to "myLastOpTime", returns true.
     *
     * "now" is used to skip over currently blacklisted sync sources.
     */
    virtual bool shouldChangeSyncSource(const HostAndPort& currentSource,
                                        const rpc::ReplSetMetadata& metadata) = 0;

    /**
     * Returns a SyncSourceResolverResponse containing the syncSource or a new MinValid boundry as
     * described in SyncSourceResolverResponse.
     */
    virtual SyncSourceResolverResponse selectSyncSource(OperationContext* txn,
                                                        const OpTime& lastOpTimeFetched) = 0;
};

}  // namespace repl
}  // namespace mongo
