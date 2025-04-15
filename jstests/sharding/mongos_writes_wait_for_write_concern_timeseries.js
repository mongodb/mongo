/**
 * Tests that commands that accept write concern correctly return write concern errors when run
 * through mongos on timeseries views.
 *
 * @tags: [
 * assumes_balancer_off,
 * does_not_support_stepdowns,
 * multiversion_incompatible,
 * requires_timeseries,
 * incompatible_aubsan,
 * tsan_incompatible,
 * requires_persistence,
 * ]
 *
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    checkWriteConcernBehaviorAdditionalCRUDOps,
    checkWriteConcernBehaviorForAllCommands
} from "jstests/libs/write_concern_all_commands.js";

const overrideInternalWriteConcernTimeout = {
    setParameter: {
        'failpoint.overrideInternalWriteConcernTimeout':
            tojson({mode: 'alwaysOn', data: {wtimeoutMillis: 5000}})
    }
};
const stOtherOptions = {
    mongosOptions: overrideInternalWriteConcernTimeout,
    rsOptions: overrideInternalWriteConcernTimeout,
    configOptions: overrideInternalWriteConcernTimeout,
    enableBalancer: false
};

const st = new ShardingTest({mongos: 1, shards: 2, rs: {nodes: 3}, other: stOtherOptions});
assert.commandWorked(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {"level": "local"}}));

const precmdShardKeyTimeseriesSubFieldX = function(conn, cluster, dbName, collName) {
    let db = conn.getDB(dbName);
    let nss = dbName + "." + collName;

    const shardKey = {"meta.x": 1};
    assert.commandWorked(db.adminCommand({enableSharding: dbName}));
    assert.commandWorked(db.adminCommand(
        {shardCollection: nss, key: shardKey, timeseries: {timeField: "time", metaField: "meta"}}));
};

const precmdShardKeyTimeseriesSubFieldZ = function(conn, cluster, dbName, collName) {
    let db = conn.getDB(dbName);
    let nss = dbName + "." + collName;

    const shardKey = {"meta.z": 1};
    assert.commandWorked(db.adminCommand({enableSharding: dbName}));
    assert.commandWorked(db.adminCommand(
        {shardCollection: nss, key: shardKey, timeseries: {timeField: "time", metaField: "meta"}}));
};

const precmdUnshardedTimeseries = function(conn, cluster, dbName, collName) {
    let db = conn.getDB(dbName);
    assert.commandWorked(
        db.runCommand({create: collName, timeseries: {timeField: "time", metaField: "meta"}}));
};

jsTest.log("Testing all commands on a sharded timeseries collection with meta.x shard key.");
checkWriteConcernBehaviorForAllCommands(st.s,
                                        st,
                                        "sharded" /* clusterType */,
                                        precmdShardKeyTimeseriesSubFieldX,
                                        true /* shardedCollection */,
                                        true /* limitToTimeseriesViews */);

jsTest.log("Testing all commands on a sharded timeseries collection with meta.z shard key.");
checkWriteConcernBehaviorForAllCommands(st.s,
                                        st,
                                        "sharded" /* clusterType */,
                                        precmdShardKeyTimeseriesSubFieldZ,
                                        true /* shardedCollection */,
                                        true /* limitToTimeseriesViews */);

jsTest.log("Testing all commands on an unsharded timeseries collection.");
checkWriteConcernBehaviorForAllCommands(st.s,
                                        st,
                                        "sharded" /* clusterType */,
                                        precmdUnshardedTimeseries,
                                        false /* shardedCollection */,
                                        true /* limitToTimeseriesViews */);

jsTest.log(
    "Testing additional CRUD commands on a sharded timeseries collection. The writes will take the target without shard key path.");

checkWriteConcernBehaviorAdditionalCRUDOps(st.s,
                                           st,
                                           "sharded" /* clusterType */,
                                           precmdShardKeyTimeseriesSubFieldZ,
                                           true /* shardedCollection */,
                                           true /* writeWithoutSk */,
                                           true /* limitToTimeseriesViews */);

jsTest.log(
    "Testing additional CRUD commands on a sharded timeseries collection. The writes will take the target with shard key path.");

checkWriteConcernBehaviorAdditionalCRUDOps(st.s,
                                           st,
                                           "sharded" /* clusterType */,
                                           precmdShardKeyTimeseriesSubFieldX,
                                           true /* shardedCollection */,
                                           false /* writeWithoutSk */,
                                           true /* limitToTimeseriesViews */);

jsTest.log("Testing additional CRUD commands on an unsharded timeseries collection.");

checkWriteConcernBehaviorAdditionalCRUDOps(st.s,
                                           st,
                                           "sharded" /* clusterType */,
                                           precmdUnshardedTimeseries,
                                           false /* shardedCollection */,
                                           true /* writeWithoutSk */,
                                           true /* limitToTimeseriesViews */);

st.stop();
