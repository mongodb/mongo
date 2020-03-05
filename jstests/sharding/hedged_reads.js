/*
 * Tests hedging metrics in the serverStatus output.
 * @tags: [requires_fcv_44]
 */
(function() {
"use strict";

function setCommandDelay(nodeConn, command, delay) {
    assert.commandWorked(nodeConn.adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {
            failInternalCommands: true,
            blockConnection: true,
            blockTimeMS: delay,
            failCommands: [command],
        }
    }));
}

function clearCommandDelay(nodeConn) {
    assert.commandWorked(nodeConn.adminCommand({
        configureFailPoint: "failCommand",
        mode: "off",
    }));
}

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
const dbName = "foo";
const collName = "bar";
const ns = dbName + "." + collName;
const testDB = st.s.getDB(dbName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

// Verify that MaxTimeMS expiration does not affect the command result.
try {
    setCommandDelay(st.rs0.getPrimary(), "find", 500);

    // the hedged read will have the MaxTimeMS set to 10ms, hence need to sleep longer than that.
    assert.commandWorked(testDB.runCommand({
        query: {
            find: collName,
            filter: {$where: "sleep(100); return true;"},
            $readPreference: {mode: "nearest"}
        }
    }));

} finally {
    clearCommandDelay(st.rs0.getPrimary());
}

st.stop();
}());
