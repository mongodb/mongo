/**
 * Tests that read preference metrics correctly count user-initiated commands routed through mongos
 * as "external" rather than "internal" on the shard's mongod.
 *
 * Without the isExternalClientOnRouter flag, commands arriving from mongos are counted as "internal"
 * because the mongos-to-mongod connection is an internal client. This test verifies that user
 * commands through mongos correctly increment the "external" counter on the shard.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_fcv_90,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "readPrefMetricsTest";
const collName = "testColl";
const foreignCollName = "foreignColl";

function getReadPreferenceMetrics(conn) {
    const serverStatus = assert.commandWorked(conn.getDB("admin").runCommand({serverStatus: 1}));
    assert(
        serverStatus.hasOwnProperty("readPreferenceCounters"),
        "Server status is missing 'readPreferenceCounters' field",
    );
    return serverStatus.readPreferenceCounters;
}

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
const mongos = st.s;
const primaryShard = st.rs0.getPrimary();

const mongosDB = mongos.getDB(dbName);
assert.commandWorked(mongosDB.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

// Insert documents so reads have something to find.
assert.commandWorked(mongosDB.getCollection(collName).insert({_id: 1, x: 1}));
assert.commandWorked(mongosDB.getCollection(foreignCollName).insert({_id: 1, x: 1}));

// Shard the foreign collection across both shards so that a $lookup from mongod will need to
// dispatch sub-operations to the other shard (mongod -> mongod path).
// split at {_id: 0}, move the {_id: 1} chunk to the other shard.
st.shardColl(foreignCollName, {_id: 1}, {_id: 0}, {_id: 1}, dbName);

/**
 * Verifies that a user command routed through mongos increments the "external" counter on the
 * shard. We only assert that external increments by the expected amount; we do not assert that
 * the internal counter is unchanged because unrelated background operations may legitimately
 * increment it between our pre/post serverStatus reads.
 */
function verifyExternalCounterIncrements(readPref, shardConn, executedOn, expectedIncrement) {
    if (expectedIncrement === undefined) {
        expectedIncrement = 1;
    }
    const preMet = getReadPreferenceMetrics(shardConn);

    const externalBefore = preMet[executedOn][readPref].external;

    return {
        assertIncrementedBy: function (actualIncrement) {
            if (actualIncrement === undefined) {
                actualIncrement = expectedIncrement;
            }
            const postMet = getReadPreferenceMetrics(shardConn);
            const externalAfter = postMet[executedOn][readPref].external;

            jsTestLog(
                `readPref=${readPref}, executedOn=${executedOn}: ` +
                    `external ${externalBefore} -> ${externalAfter} ` +
                    `(expected +${actualIncrement})`,
            );

            assert.eq(
                externalAfter,
                externalBefore + actualIncrement,
                `Expected external counter for ${readPref} on ${executedOn} to increment ` +
                    `by ${actualIncrement}. Got external: ${externalBefore} -> ${externalAfter}.`,
            );
        },
    };
}

jsTestLog("Testing that user commands through mongos count as external on shard primary");

for (const readPref of ["primary", "primaryPreferred"]) {
    const check = verifyExternalCounterIncrements(readPref, primaryShard, "executedOnPrimary");
    const cmd = {count: collName, $readPreference: {mode: readPref}};
    assert.commandWorked(mongosDB.runCommand(cmd));
    check.assertIncrementedBy();
}

jsTestLog("Testing that sharded $lookup sub-operations propagate the external flag");

const shard1Primary = st.rs1.getPrimary();

{
    const check = verifyExternalCounterIncrements("primary", shard1Primary, "executedOnPrimary");
    const pipeline = [{$lookup: {from: foreignCollName, localField: "x", foreignField: "x", as: "matched"}}];
    assert.commandWorked(
        mongosDB.runCommand({
            aggregate: collName,
            pipeline: pipeline,
            cursor: {},
            $readPreference: {mode: "primary"},
        }),
    );
    check.assertIncrementedBy();
}

jsTestLog("Testing that $lookup sub-operations during getMore propagate the external flag");

assert.commandWorked(mongosDB.getCollection(collName).insert({_id: 2, x: 2}));
assert.commandWorked(mongosDB.getCollection(foreignCollName).insert({_id: 2, x: 2}));

{
    const check = verifyExternalCounterIncrements("primary", shard1Primary, "executedOnPrimary", 2);
    const pipeline = [{$lookup: {from: foreignCollName, localField: "x", foreignField: "x", as: "matched"}}];
    const cursor = mongosDB.getCollection(collName).aggregate(pipeline, {batchSize: 1});
    cursor.toArray();
    check.assertIncrementedBy();
}

st.stop();
