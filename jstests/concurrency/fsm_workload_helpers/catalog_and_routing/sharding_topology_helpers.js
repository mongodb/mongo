import {
    getReplSetName,
    makeNewConnWithExistingSession,
    makeReplSetConnWithExistingSession,
} from "jstests/concurrency/fsm_utils/connection_utils.js";

export var ShardingTopologyHelpers = (function () {
    const kNonGrpcConnStr = (connStr) =>
        jsTestOptions().shellGRPC ? `mongodb://${connStr}/?grpc=false` : `mongodb://${connStr}`;

    function getShardInfo(mongosConn, tid) {
        // Use the session from the mongoS, the fsm libs will take care of propogating it to the mongoS.
        const makeNewConnWithCurrentSession = function (connStr) {
            return makeNewConnWithExistingSession(connStr, mongosConn.getSession());
        };

        let shardInfo = {shards: {}, rsConns: {}};
        const shards = assert.commandWorked(mongosConn.adminCommand({listShards: 1})).shards;
        shards.forEach((shard) => {
            let [_, hostString] = shard.host.split("/");
            let hostList = hostString.split(",");
            shardInfo.shards[shard._id] = hostList
                .map((connStr) => kNonGrpcConnStr(connStr))
                .map(makeNewConnWithCurrentSession);
            shardInfo.rsConns[shard._id] = makeReplSetConnWithExistingSession(
                hostList,
                getReplSetName(shardInfo.shards[shard._id][0]),
                tid,
                mongosConn.getSession(),
            );
        });
        return shardInfo;
    }

    function executeWithShardInfo(mongosConn, tid, func) {
        assert.soon(() => {
            const shardInfo = getShardInfo(mongosConn, tid);
            try {
                func(shardInfo);
                return true;
            } catch (e) {
                if (TestData.shardsAddedRemoved && e.code == ErrorCodes.ShardNotFound) {
                    return false;
                }
                throw e;
            }
        });
    }

    function getShardNames(mongosConn) {
        return assert.commandWorked(mongosConn.adminCommand({listShards: 1})).shards.map((shard) => {
            return shard._id;
        });
    }

    return {
        executeWithShardInfo: executeWithShardInfo,
        getShardNames: getShardNames,
        getShardInfo: getShardInfo,
    };
})();
