/**
 * Tests that opcounters.command increases appropriately for insert, delete and update.
 *
 * @tags: [
 *   requires_fcv_62,
 *   # TODO (SERVER-88123): Re-enable this test.
 *   # Test doesn't start enough mongods to have num_mongos routers
 *   embedded_router_incompatible,
 * ]
 */

const st = new ShardingTest({mongos: 2, shards: 1});

function getOpCounters(conn) {
    const opcounters = assert.commandWorked(conn.adminCommand({serverStatus: 1})).opcounters;
    jsTest.log("opcounters " + tojson(opcounters));
    return opcounters;
}

const dbName = "testDb";
const collName = "testColl";
const mongosDB = st.s.getDB(dbName);
const cnt = 100;

{
    const opCountersBefore = getOpCounters(st.s);
    for (let i = 1; i <= cnt; ++i) {
        assert.commandWorked(mongosDB.runCommand({insert: collName, documents: [{x: 0}]}));
    }
    const opCountersAfter = getOpCounters(st.s);

    assert.gte(opCountersAfter.insert, opCountersBefore.insert + cnt);
    // "command" should only increase by at least 1 (i.e. count only the 'serverStatus' command).
    // There can be commands from config server to mongos which can increase the command count.
    assert.gt(opCountersAfter.command, opCountersBefore.command);
}

{
    const opCountersBefore = getOpCounters(st.s);
    for (let i = 1; i <= cnt; ++i) {
        assert.commandWorked(
            mongosDB.runCommand({update: collName, updates: [{q: {x: 0}, u: {$set: {y: 0}}}]}));
    }
    const opCountersAfter = getOpCounters(st.s);
    assert.gte(opCountersAfter.update, opCountersBefore.update + cnt);
    // "command" should only increase by at least 1 (i.e. count only the 'serverStatus' command).
    // There can be commands from config server to mongos which can increase the command count.
    assert.gt(opCountersAfter.command, opCountersBefore.command);
}

{
    const opCountersBefore = getOpCounters(st.s);
    for (let i = 1; i <= cnt; ++i) {
        assert.commandWorked(
            mongosDB.runCommand({delete: collName, deletes: [{q: {x: 0}, limit: 0}]}));
    }
    const opCountersAfter = getOpCounters(st.s);

    assert.gte(opCountersAfter.delete, opCountersBefore.delete + cnt);
    // "command" should only increase by at least 1 (i.e. count only the 'serverStatus' command).
    // There can be commands from config server to mongos which can increase the command count.
    assert.gt(opCountersAfter.command, opCountersBefore.command);
}

st.stop();
