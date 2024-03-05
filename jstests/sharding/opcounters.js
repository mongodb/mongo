/**
 * Tests that opcounters.command on mongos doesn't count inserts, updates and deletes.
 *
 * @tags: [
 *   requires_fcv_62,
 *   # Test doesn't start enough mongods to have num_mongos routers
 *   temp_disabled_embedded_router,
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

{
    const opCountersBefore = getOpCounters(st.s);
    assert.commandWorked(mongosDB.runCommand({insert: collName, documents: [{x: 0}]}));
    const opCountersAfter = getOpCounters(st.s);

    assert.eq(opCountersAfter.insert, opCountersBefore.insert + 1);
    // "command" should only increase by 1 (i.e. count only the 'serverStatus' command).
    assert.eq(opCountersAfter.command, opCountersBefore.command + 1);
}

{
    const opCountersBefore = getOpCounters(st.s);
    assert.commandWorked(
        mongosDB.runCommand({update: collName, updates: [{q: {x: 0}, u: {$set: {y: 0}}}]}));
    const opCountersAfter = getOpCounters(st.s);

    assert.eq(opCountersAfter.update, opCountersBefore.update + 1);
    // "command" should only increase by 1 (i.e. count only the 'serverStatus' command).
    assert.eq(opCountersAfter.command, opCountersBefore.command + 1);
}

{
    const opCountersBefore = getOpCounters(st.s);
    assert.commandWorked(mongosDB.runCommand({delete: collName, deletes: [{q: {x: 0}, limit: 0}]}));
    const opCountersAfter = getOpCounters(st.s);

    assert.eq(opCountersAfter.delete, opCountersBefore.delete + 1);
    // "command" should only increase by 1 (i.e. count only the 'serverStatus' command).
    assert.eq(opCountersAfter.command, opCountersBefore.command + 1);
}

st.stop();