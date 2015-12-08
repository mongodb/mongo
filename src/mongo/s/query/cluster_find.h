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

#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/explain_common.h"

namespace mongo {

template <typename T>
class StatusWith;
class CanonicalQuery;
class OperationContext;
struct GetMoreRequest;
struct ReadPreferenceSetting;

namespace rpc {
class ServerSelectionMetadata;
}  // namespace rpc

/**
 * Methods for running find and getMore operations across a sharded cluster.
 */
class ClusterFind {
public:
    // The number of times we are willing to re-target and re-run the query after receiving a stale
    // config message.
    static const size_t kMaxStaleConfigRetries;

    /**
     * Runs query 'query', targeting remote hosts according to the read preference in 'readPref'.
     *
     * On success, fills out 'results' with the first batch of query results and returns the cursor
     * id which the caller can use on subsequent getMore operations. If no cursor needed to be saved
     * (e.g. the cursor was exhausted without need for a getMore), returns a cursor id of 0.
     */
    static StatusWith<CursorId> runQuery(OperationContext* txn,
                                         const CanonicalQuery& query,
                                         const ReadPreferenceSetting& readPref,
                                         std::vector<BSONObj>* results);

    /**
     * Executes the getMore request 'request', and on success returns a CursorResponse.
     */
    static StatusWith<CursorResponse> runGetMore(OperationContext* txn,
                                                 const GetMoreRequest& request);

    /**
     * Extracts the read preference from 'cmdObj', or determines the read pref based on 'isSlaveOk'
     * if 'cmdObj' does not contain a read preference.
     *
     * Expects a read preference that has already been "unwrapped" by the mongos command handling
     * code, e.g. { ... , $queryOptions: { $readPreference: { ... } } , ... }.
     *
     * Returns a non-OK status if 'cmdObj' has a read preference but the read preference does not
     * parse correctly.
     */
    static StatusWith<ReadPreferenceSetting> extractUnwrappedReadPref(const BSONObj& cmdObj,
                                                                      bool isSlaveOk);
};

}  // namespace mongo
