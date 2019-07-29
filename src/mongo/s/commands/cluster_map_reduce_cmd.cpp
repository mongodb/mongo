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

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/db/commands.h"
#include "mongo/db/commands/map_reduce_command_base.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/s/commands/cluster_map_reduce.h"
#include "mongo/s/commands/cluster_map_reduce_agg.h"

namespace mongo {
namespace {

/**
 * Outline for sharded map reduce for sharded output, $out replace:
 *
 * ============= mongos =============
 * 1. Send map reduce command to all relevant shards with some extra info like the value for
 *    the chunkSize and the name of the temporary output collection.
 *
 * ============= shard =============
 * 2. Does normal map reduce.
 *
 * 3. Calls splitVector on itself against the output collection and puts the results into the
 *    response object.
 *
 * ============= mongos =============
 * 4. If the output collection is *not* sharded, uses the information from splitVector to
 *    create a pre-split sharded collection.
 *
 * 5. Grabs the distributed lock for the final output collection.
 *
 * 6. Sends mapReduce.shardedfinish.
 *
 * ============= shard =============
 * 7. Extracts the list of shards from the mapReduce.shardedfinish and performs a broadcast
 *    query against all of them to obtain all documents that this shard owns.
 *
 * 8. Performs the reduce operation against every document from step #7 and outputs them to
 *    another temporary collection. Also keeps track of the BSONObject size of every "reduced"
 *    document for each chunk range.
 *
 * 9. Atomically drops the old output collection and renames the temporary collection to the
 *    output collection.
 *
 * ============= mongos =============
 * 10. Releases the distributed lock acquired at step #5.
 *
 * 11. Inspects the BSONObject size from step #8 and determines if it needs to split.
 */
class ClusterMapReduceCommand : public MapReduceCommandBase {
public:
    ClusterMapReduceCommand() = default;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool _runImpl(OperationContext* opCtx,
                  const std::string& dbname,
                  const BSONObj& cmd,
                  std::string& errmsg,
                  BSONObjBuilder& result) final {
        if (getTestCommandsEnabled() && internalQueryUseAggMapReduce.load()) {
            return runAggregationMapReduce(opCtx, dbname, cmd, errmsg, result);
        }
        return runMapReduce(opCtx, dbname, cmd, errmsg, result);
    }
} clusterMapReduceCommand;

}  // namespace
}  // namespace mongo
