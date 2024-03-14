/**
 * Tests that the transaction API can be used for distributed transactions initiated from a shard.
 *
 * @tags: [requires_fcv_60]
 */
// The test command is meant to test the "no session" transaction API case.
TestData.disableImplicitSessions = true;

const st = new ShardingTest({shards: 2, config: 1});
const shard0Primary = st.rs0.getPrimary();

const kDbName = "foo";
const kCollName = "bar";
const kNs = kDbName + "." + kCollName;

assert.commandWorked(
    st.s.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}));

function runTestSuccess(sessionOpts) {
    const commands = [
        {dbName: kDbName, command: {find: kCollName, singleBatch: true}},
        {
            dbName: kDbName,
            command: {insert: kCollName, documents: [{_id: 2}, {_id: 3}], stmtId: NumberInt(0)}
        },
        {
            dbName: kDbName,
            command: {
                update: kCollName,
                updates: [{q: {_id: 2}, u: {$set: {updated: true}}}],
                stmtId: NumberInt(2)
            }
        },
        {
            dbName: kDbName,
            command: {delete: kCollName, deletes: [{q: {_id: 3}, limit: 1}], stmtId: NumberInt(3)}
        },
        {dbName: kDbName, command: {find: kCollName, singleBatch: true}},
        {dbName: kDbName, command: {aggregate: kCollName, pipeline: [{$match: {}}], cursor: {}}},
    ];

    // Insert initial data.
    assert.commandWorked(st.s.getCollection(kNs).insert([{_id: 1}]));

    let testCmd = Object.merge(
        {testInternalTransactions: 1, commandInfos: commands, useClusterClient: true}, sessionOpts);
    const res = assert.commandWorked(shard0Primary.adminCommand(testCmd));
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

function runTestFailure(sessionOpts) {
    const commands = [
        {
            dbName: kDbName,
            command: {insert: kCollName, documents: [{_id: 2}, {_id: 3}], stmtId: NumberInt(0)}
        },
        {dbName: kDbName, command: {find: kCollName, singleBatch: true}},
        // clusterCount does not exist, so the API will reject this command without running it. This
        // will still abort the transaction.
        {dbName: kDbName, command: {count: kCollName}},
    ];

    // Insert initial data.
    assert.commandWorked(st.s.getCollection(kNs).insert([{_id: 1}]));

    let testCmd = Object.merge(
        {testInternalTransactions: 1, commandInfos: commands, useClusterClient: true}, sessionOpts);

    // TODO (SERVER-73632): Simplify this path once 8.0 becomes last LTS.
    const binVersion = assert.commandWorked(shard0Primary.adminCommand({serverStatus: 1}));
    let errorCode = MongoRunner.compareBinVersions(binVersion.version, "8.0") >= 0
        ? ErrorCodes.OperationNotSupportedInTransaction
        : 6349501;
    const res = assert.commandFailedWithCode(shard0Primary.adminCommand(testCmd), errorCode);
    assert(!res.hasOwnProperty("responses"));

    // Verify the API didn't insert any documents.
    assert.sameMembers(st.s.getCollection(kNs).find().toArray(), [{_id: 1}]);

    // Clean up.
    assert.commandWorked(st.s.getCollection(kNs).remove({}, false /* justOne */));
}

function runTestGetMore(sessionOpts) {
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

    let testCmd = Object.merge(
        {testInternalTransactions: 1, commandInfos: commands, useClusterClient: true}, sessionOpts);
    const res = assert.commandWorked(shard0Primary.adminCommand(testCmd));
    assert.eq(res.responses.length, 1, tojson(res));

    // The response from an exhausted cursor is an array of BSON objects, so we don't assert the
    // command worked.
    assert.eq(res.responses[0].docs.length, numDocs, tojson(res));
    for (let i = 0; i < numDocs; ++i) {
        assert.eq(res.responses[0].docs[i]._id, startVal + i, tojson(res.responses[0].docs[i]));
    }

    // Verify getMores were used by checking serverStatus metrics.
    const commandMetricsAfter = shard0Primary.getDB(kDbName).serverStatus().metrics.commands;

    // TODO (SERVER-73632): Simplify this path once 8.0 becomes last LTS.
    const binVersion = assert.commandWorked(shard0Primary.adminCommand({serverStatus: 1}));
    if (MongoRunner.compareBinVersions(binVersion.version, "8.0") >= 0) {
        assert.gt(commandMetricsAfter.find.total, commandMetricsBefore.find.total);
        if (!commandMetricsBefore.getMore) {
            // The unsharded case runs before any cluster getMores are run.
            assert.gt(commandMetricsAfter.getMore.total, 0);
        } else {
            assert.gt(commandMetricsAfter.getMore.total, commandMetricsBefore.getMore.total);
        }
    } else {
        assert.gt(commandMetricsAfter.clusterFind.total, commandMetricsBefore.clusterFind.total);
        if (!commandMetricsBefore.clusterGetMore) {
            // The unsharded case runs before any cluster getMores are run.
            assert.gt(commandMetricsAfter.clusterGetMore.total, 0);
        } else {
            assert.gt(commandMetricsAfter.clusterGetMore.total,
                      commandMetricsBefore.clusterGetMore.total);
        }
    }

    // Clean up.
    assert.commandWorked(st.s.getCollection(kNs).remove({}, false /* justOne */));
}

//
// Unsharded collection case.
//

runTestSuccess({});
runTestSuccess({lsid: {id: new UUID()}});
runTestSuccess({lsid: {id: new UUID()}, txnNumber: NumberLong(0)});

runTestFailure({});
runTestFailure({lsid: {id: new UUID()}});
runTestFailure({lsid: {id: new UUID()}, txnNumber: NumberLong(0)});

runTestGetMore({});
runTestGetMore({lsid: {id: new UUID()}});
runTestGetMore({lsid: {id: new UUID()}, txnNumber: NumberLong(0)});

//
// Sharded collection case.
//
assert.commandWorked(st.s.getCollection(kNs).createIndex({x: 1}));
assert.commandWorked(st.s.adminCommand({shardCollection: kNs, key: {x: 1}}));

assert.commandWorked(st.s.adminCommand({split: kNs, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: kNs, find: {x: 0}, to: st.shard1.shardName}));

runTestSuccess({});
runTestSuccess({lsid: {id: new UUID()}});
runTestSuccess({lsid: {id: new UUID()}, txnNumber: NumberLong(0)});

runTestFailure({});
runTestFailure({lsid: {id: new UUID()}});
runTestFailure({lsid: {id: new UUID()}, txnNumber: NumberLong(0)});

runTestGetMore({});
runTestGetMore({lsid: {id: new UUID()}});
runTestGetMore({lsid: {id: new UUID()}, txnNumber: NumberLong(0)});

st.stop();
