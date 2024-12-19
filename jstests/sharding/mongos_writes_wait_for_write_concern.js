// Sharding version of jstests/replsets/noop_writes_wait_for_write_concern.js.
// @tags: [
//   multiversion_incompatible,
// ]

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    getWriteCommands,
    getWriteCommandsForShardedCollection
} from "jstests/libs/write_commands.js";

// Create a shard with 3 nodes and stop one of the secondaries. This will allow majority write
// concern to be met, but w: 3 will always time out.
var st = new ShardingTest({mongos: 1, shards: 2, rs: {nodes: 3}});
const secondary0 = st.rs0.getSecondary();
st.rs0.stop(secondary0);

const mongos = st.s;
const dbName = 'testDB';
const db = mongos.getDB(dbName);
const collName = 'testColl';
const coll = db[collName];
const commands = getWriteCommands(coll, "sharding-unsharded");

const shardingDbName = 'shardedDB';
const dbSh = mongos.getDB(shardingDbName);
assert.commandWorked(
    dbSh.adminCommand({enableSharding: shardingDbName, primary: st.shard0.shardName}));
const shCollName = 'shardedTestColl';
const shColl = dbSh[shCollName];

// Setup sharded collection.
const shNs = shardingDbName + "." + shCollName;
const precmd = () => {
    assert.commandWorked(db.adminCommand({shardCollection: shNs, key: {_id: 1}}));
    assert.commandWorked(db.adminCommand({split: shNs, middle: {_id: 0}}));
    assert.commandWorked(
        db.adminCommand({moveChunk: shNs, find: {_id: 1}, to: st.shard1.shardName}));
};

const shCommands = getWriteCommands(shColl, "sharding-sharded", precmd)
                       .concat(getWriteCommandsForShardedCollection(shColl, precmd));

function testCommandWithWriteConcern(cmd, coll) {
    if ("applyOps" in cmd.req) {
        // applyOps is not available through mongos.
        return;
    }

    if ("dropIndexes" in cmd.req) {
        // TODO SERVER-97754: dropIndexes does not return write concern errors.
        jsTest.log("Skipping dropIndexes test: no WCE in mongos, SERVER-97754.");
        return;
    }

    if (cmd.req["dropDatabase"] !== undefined || cmd.req["drop"] !== undefined) {
        // TODO SERVER-97754: dropDatabase and dropCollection do not return write concern errors.
        jsTest.log("Skipping dropDatabase/drop test: no WCE in mongos, SERVER-97754.");
        return;
    }

    if ("create" in cmd.req && !("createIndexes" in cmd.req)) {
        // TODO SERVER-73553: create returns WriteConcernTimeout as an ordinary error code instead
        // of using the writeConcernError field.
        jsTest.log("Skipping create/createIndexes test: no WCE in mongos, SERVER-73553.");
        return;
    }

    if ("renameCollection" in cmd.req) {
        // TODO SERVER-97754: WriteConcernError is swallowed when the command is sent to mongos.
        jsTest.log("Skipping renameCollection test: no WCE in mongos, SERVER-97754.");
        return;
    }

    const conn = new Mongo(mongos.host);
    cmd.run(coll, conn);
}

jsTest.log("Testing commands on unsharded collection through mongos.");
commands.forEach(function(cmd) {
    testCommandWithWriteConcern(cmd, coll);
});

jsTest.log("Testing commands on sharded collection (multiple shards) through mongos.");
shCommands.forEach(function(cmd) {
    testCommandWithWriteConcern(cmd, shColl);
});

// Restart the node so that consistency checks performed by st.stop() can succeed.
st.rs0.restart(secondary0);
st.stop();
