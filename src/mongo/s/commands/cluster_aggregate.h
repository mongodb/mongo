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

#pragma once

#include <memory>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_merge_cursors.h"
#include "mongo/s/commands/strategy.h"

namespace mongo {

class OperationContext;
class ShardId;

/**
 * Methods for running aggregation across a sharded cluster.
 */
class ClusterAggregate {
public:
    /**
     * 'requestedNss' is the namespace aggregation will register cursors under. This is the
     * namespace which we will return in responses to aggregate / getMore commands, and it is the
     * namespace we expect users to hand us inside any subsequent getMores. 'executionNss' is the
     * namespace we will run the mongod aggregate and subsequent getMore's against.
     */
    struct Namespaces {
        NamespaceString requestedNss;
        NamespaceString executionNss;
    };

    /**
     * Executes the aggregation 'request' using context 'opCtx'.
     *
     * The 'namespaces' struct should contain both the user-requested namespace and the namespace
     * over which the aggregation will actually execute. Typically these two namespaces are the
     * same, but they may differ in the case of a query on a view.
     *
     * The raw aggregate command parameters should be passed in 'cmdObj'.
     *
     * On success, fills out 'result' with the command response.
     */
    static Status runAggregate(OperationContext* opCtx,
                               const Namespaces& namespaces,
                               const AggregationRequest& request,
                               BSONObj cmdObj,
                               int options,
                               BSONObjBuilder* result);

private:
    static std::vector<DocumentSourceMergeCursors::CursorDescriptor> parseCursors(
        const std::vector<Strategy::CommandResult>& shardResults);

    static void killAllCursors(const std::vector<Strategy::CommandResult>& shardResults);
    static void uassertAllShardsSupportExplain(
        const std::vector<Strategy::CommandResult>& shardResults);

    // These are temporary hacks because the runCommand method doesn't report the exact
    // host the command was run on which is necessary for cursor support. The exact host
    // could be different from conn->getServerAddress() for connections that map to
    // multiple servers such as for replica sets. These also take care of registering
    // returned cursors.
    static BSONObj aggRunCommand(OperationContext* opCtx,
                                 DBClientBase* conn,
                                 const Namespaces& namespaces,
                                 const AggregationRequest& aggRequest,
                                 BSONObj cmd,
                                 int queryOptions);

    static Status aggPassthrough(OperationContext* opCtx,
                                 const Namespaces& namespaces,
                                 const ShardId& shardId,
                                 const AggregationRequest& aggRequest,
                                 BSONObj cmd,
                                 BSONObjBuilder* result,
                                 int queryOptions);
};

}  // namespace mongo
