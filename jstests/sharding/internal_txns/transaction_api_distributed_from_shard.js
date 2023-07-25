/**
 * Tests that the transaction API can be used for distributed transactions initiated from a shard.
 *
 * @tags: [requires_fcv_60]
 */
(function() {
"use strict";

// The test command is meant to test the "no session" transaction API case.
TestData.disableImplicitSessions = true;

const st = new ShardingTest({shards: 2, config: 1});
const shard0Primary = st.rs0.getPrimary();

const kDbName = "foo";
const kCollName = "bar";
const kNs = kDbName + "." + kCollName;

function runTestSuccess() {
    const commands = [
        {dbName: kDbName, command: {find: kCollName, singleBatch: true}},
        {dbName: kDbName, command: {insert: kCollName, documents: [{_id: 2}, {_id: 3}]}},
        {
            dbName: kDbName,
            command: {update: kCollName, updates: [{q: {_id: 2}, u: {$set: {updated: true}}}]}
        },
        {dbName: kDbName, command: {delete: kCollName, deletes: [{q: {_id: 3}, limit: 1}]}},
        {dbName: kDbName, command: {find: kCollName, singleBatch: true}},
        {dbName: kDbName, command: {aggregate: kCollName, pipeline: [{$match: {}}], cursor: {}}},
    ];

    // Insert initial data.
    assert.commandWorked(st.s.getCollection(kNs).insert([{_id: 1}]));

    const res = assert.commandWorked(shard0Primary.adminCommand(
        {testInternalTransactions: 1, commandInfos: commands, useClusterClient: true}));
    res.responses.forEach((innerRes) => {
        assert.commandWorked(innerRes, tojson(res));
    });

    assert.eq(res.responses.length, commands.length, tojson(res));
    assert.sameMembers(res.responses[0].cursor.firstBatch, [{_id: 1}], tojson(res));
    assert.eq(res.responses[1], {n: 2, ok: 1}, tojson(res));
    assert.eq(res.responses[2], {nModified: 1, n: 1, ok: 1}, tojson(res));
    assert.eq(res.responses[3], {n: 1, ok: 1}, tojson(res));
    assert.sameMembers(
        res.responses[4].cursor.firstBatch, [{_id: 1}, {_id: 2, updated: true}], tojson(res));
    assert.eq(res.responses[4].cursor.id, 0, tojson(res));
    assert.sameMembers(
        res.responses[5].cursor.firstBatch, [{_id: 1}, {_id: 2, updated: true}], tojson(res));
    assert.eq(res.responses[5].cursor.id, 0, tojson(res));

    // The written documents should be visible outside the transaction.
    assert.sameMembers(st.s.getCollection(kNs).find().toArray(),
                       [{_id: 1}, {_id: 2, updated: true}]);

    // Clean up.
    assert.commandWorked(st.s.getCollection(kNs).remove({}, false /* justOne */));
}

function runTestFailure() {
    const commands = [
        {dbName: kDbName, command: {insert: kCollName, documents: [{_id: 2}, {_id: 3}]}},
        {dbName: kDbName, command: {find: kCollName, singleBatch: true}},
        // clusterCount does not exist, so the API will reject this command without running it. This
        // will still abort the transaction.
        {dbName: kDbName, command: {count: kCollName}},
    ];

    // Insert initial data.
    assert.commandWorked(st.s.getCollection(kNs).insert([{_id: 1}]));

    const res = assert.commandFailedWithCode(
        shard0Primary.adminCommand(
            {testInternalTransactions: 1, commandInfos: commands, useClusterClient: true}),
        6349501);
    assert(!res.hasOwnProperty("responses"));

    // Verify the API didn't insert any documents.
    assert.sameMembers(st.s.getCollection(kNs).find().toArray(), [{_id: 1}]);

    // Clean up.
    assert.commandWorked(st.s.getCollection(kNs).remove({}, false /* justOne */));
}

function runTestGetMore() {
    // Insert initial data.
    const startVal = -50;
    const numDocs = 100;

    let bulk = st.s.getCollection(kNs).initializeUnorderedBulkOp();
    for (let i = startVal; i < startVal + numDocs; i++) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());

    const commands = [
        // Use a batch size < number of documents so the API must use getMores to exhaust the
        // cursor.
        {dbName: kDbName, command: {find: kCollName, batchSize: 17}, exhaustCursor: true},
    ];

    const commandMetricsBefore = shard0Primary.getDB(kDbName).serverStatus().metrics.commands;

    const res = assert.commandWorked(shard0Primary.adminCommand(
        {testInternalTransactions: 1, commandInfos: commands, useClusterClient: true}));
    assert.eq(res.responses.length, 1, tojson(res));

    // The response from an exhausted cursor is an array of BSON objects, so we don't assert the
    // command worked.
    assert.eq(res.responses[0].docs.length, numDocs, tojson(res));
    for (let i = 0; i < numDocs; ++i) {
        assert.eq(res.responses[0].docs[i]._id, startVal + i, tojson(res.responses[0].docs[i]));
    }

    // Verify getMores were used by checking serverStatus metrics.
    const commandMetricsAfter = shard0Primary.getDB(kDbName).serverStatus().metrics.commands;

    assert.gt(commandMetricsAfter.clusterFind.total, commandMetricsBefore.clusterFind.total);
    if (!commandMetricsBefore.clusterGetMore) {
        // The unsharded case runs before any cluster getMores are run.
        assert.gt(commandMetricsAfter.clusterGetMore.total, 0);
    } else {
        assert.gt(commandMetricsAfter.clusterGetMore.total,
                  commandMetricsBefore.clusterGetMore.total);
    }

    // Clean up.
    assert.commandWorked(st.s.getCollection(kNs).remove({}, false /* justOne */));
}

//
// Unsharded collection case.
//

runTestSuccess();
runTestFailure();
runTestGetMore();

//
// Sharded collection case.
//

assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, st.shard0.shardName);
assert.commandWorked(st.s.getCollection(kNs).createIndex({x: 1}));
assert.commandWorked(st.s.adminCommand({shardCollection: kNs, key: {x: 1}}));

assert.commandWorked(st.s.adminCommand({split: kNs, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: kNs, find: {x: 0}, to: st.shard1.shardName}));

runTestSuccess();
runTestFailure();
runTestGetMore();

st.stop();
})();
