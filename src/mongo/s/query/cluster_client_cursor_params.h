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

#include <boost/optional.hpp>
#include <memory>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/client/shard.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class OperationContext;

struct ClusterClientCursorParams {
    // When mongos has to do a merge in order to return results to the client in the correct sort
    // order, it requests a sortKey meta-projection using this field name.
    static const char kSortKeyField[];

    /**
     * Contains any CCC parameters that are specified per-remote node.
     */
    struct Remote {
        /**
         * Use when a new cursor should be created on the remote.
         */
        Remote(ShardId sid, BSONObj cmdObj) : shardId(std::move(sid)), cmdObj(std::move(cmdObj)) {}

        /**
         * Use when an a cursor already exists on the remote.  The resulting CCC will take ownership
         * of the existing remote cursor, generating results based on its current state.
         *
         * Note that any results already generated from this cursor will not be returned by the
         * resulting CCC.  The caller is responsible for ensuring that results previously generated
         * by this cursor have been processed.
         */
        Remote(HostAndPort hostAndPort, CursorId cursorId)
            : hostAndPort(std::move(hostAndPort)), cursorId(cursorId) {}

        // If this is a regular query cursor, this value will be set and shard id retargeting may
        // occur on certain networking or replication errors.
        //
        // If this is an externally-prepared cursor (as is in the case of aggregation cursors),
        // this value will never be set and no retargeting will occur.
        boost::optional<ShardId> shardId;

        // If this is an externally-specified cursor (e.g. aggregation), this value will be set and
        // used directly and no re-targeting may happen on errors.
        boost::optional<HostAndPort> hostAndPort;

        // The raw command parameters to send to this remote (e.g. the find command specification).
        //
        // Exactly one of 'cmdObj' or 'cursorId' must be set.
        boost::optional<BSONObj> cmdObj;

        // The cursorId for the remote node, if one already exists.
        //
        // Exactly one of 'cmdObj' or 'cursorId' must be set.
        boost::optional<CursorId> cursorId;
    };

    /**
     * Constructor used for cases where initial shard host targeting is necessary (i.e., we don't
     * know yet the remote cursor id).
     */
    ClusterClientCursorParams(NamespaceString nss, ReadPreferenceSetting readPref)
        : nsString(std::move(nss)), readPreference(std::move(readPref)) {}

    /**
     * Constructor used for cases, where the remote cursor ids are already known and no resolution
     * or retargeting needs to happen.
     */
    ClusterClientCursorParams(NamespaceString nss) : nsString(std::move(nss)) {}

    // Namespace against which to query.
    NamespaceString nsString;

    // Per-remote node data.
    std::vector<Remote> remotes;

    // The sort specification. Leave empty if there is no sort.
    BSONObj sort;

    // The number of results to skip. Optional. Should not be forwarded to the remote hosts in
    // 'cmdObj'.
    boost::optional<long long> skip;

    // The number of results per batch. Optional. If specified, will be specified as the batch for
    // each getMore.
    boost::optional<long long> batchSize;

    // Limits the number of results returned by the ClusterClientCursor to this many. Optional.
    // Should be forwarded to the remote hosts in 'cmdObj'.
    boost::optional<long long> limit;

    // Whether this cursor is tailing a capped collection.
    bool isTailable = false;

    // Whether this cursor has the awaitData option set.
    bool isAwaitData = false;

    // Read preference for where to target the query. This value is only set if initial shard host
    // targeting is necessary and not used if using externally prepared cursor ids.
    boost::optional<ReadPreferenceSetting> readPreference;

    // Whether the client indicated that it is willing to receive partial results in the case of an
    // unreachable host.
    bool isAllowPartialResults = false;
};

}  // mongo
