/**
 *    Copyright (C) 2017 MongoDB Inc.
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
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/cursor_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

class CursorResponse;

/**
 * Establishes cursors on the remote shards by issuing requests in parallel, using the readPref to
 * select a host within each shard.
 *
 * If any of the cursors fails to be established, performs cleanup by sending killCursors to any
 * cursors that were established and returns a non-OK status.
 *
 * If an OK status is returned, the ownership of the cursors is transferred to the caller. This
 * means the caller is now responsible for either exhausting the cursors or sending killCursors to
 * them.
 *
 * @param allowPartialResults: If true, unreachable hosts are ignored, and only cursors established
 *                             on reachable hosts are returned.
 * @param viewDefinition: If the namespace represents a view, an error is returned and the view
 *                        definition is stored in this parameter. Calling code can then attempt to
 *                        establish cursors against the base collection using this viewDefinition.
 */
StatusWith<std::vector<ClusterClientCursorParams::RemoteCursor>> establishCursors(
    OperationContext* opCtx,
    executor::TaskExecutor* executor,
    const NamespaceString& nss,
    const ReadPreferenceSetting readPref,
    const std::vector<std::pair<ShardId, BSONObj>>& remotes,
    bool allowPartialResults,
    BSONObj* viewDefinition);

}  // namespace mongo
