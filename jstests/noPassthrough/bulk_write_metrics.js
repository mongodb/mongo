/**
 * Test that bulkWrite emit the same metrics that corresponding calls to update, delete and insert.
 *
 * @tags: [featureFlagBulkWriteCommand] // TODO SERVER-52419: Remove this tag.
 */

import {BulkWriteMetricChecker} from "jstests/libs/bulk_write_utils.js";

function runTest(isMongos, cluster, bulkWrite, retryCount) {
    // We are ok with the randomness here since we clearly log the state.
    const errorsOnly = Math.random() < 0.5;
    print(`Running on a ${isMongos ? "ShardingTest" : "ReplSetTest"} with bulkWrite = ${
        bulkWrite} and errorsOnly = ${errorsOnly}.`);

    const dbName = "testDB";
    const collName = "testColl";
    const namespace = `${dbName}.${collName}`;
    const session = isMongos ? cluster.s.startSession() : cluster.getPrimary().startSession();
    const testDB = session.getDatabase(dbName);

    if (isMongos) {
        assert.commandWorked(testDB.adminCommand({'enableSharding': dbName}));
        assert.commandWorked(testDB.adminCommand({shardCollection: namespace, key: {y: 1}}));
        assert.commandWorked(testDB.adminCommand({split: namespace, middle: {y: 5}}));
        assert.commandWorked(testDB.adminCommand({split: namespace, middle: {y: 200}}));

        // Move chunks so each shard has one chunk.
        assert.commandWorked(testDB.adminCommand({
            moveChunk: namespace,
            find: {y: 2},
            to: cluster.shard0.shardName,
            _waitForDelete: true
        }));
        assert.commandWorked(testDB.adminCommand({
            moveChunk: namespace,
            find: {y: 5},
            to: cluster.shard1.shardName,
            _waitForDelete: true
        }));
        assert.commandWorked(testDB.adminCommand({
            moveChunk: namespace,
            find: {y: 200},
            to: cluster.shard2.shardName,
            _waitForDelete: true
        }));
    }

    const coll = testDB[collName];

    // Simplifies implementation of checkBulkWriteMetrics:
    // totals["testDB.testColl"] will not be undefined on first top call below.
    assert.commandWorked(coll.insert({_id: 99}));

    const metricChecker = new BulkWriteMetricChecker(
        testDB, namespace, bulkWrite, isMongos, false /*fle*/, errorsOnly, retryCount);

    metricChecker.checkMetrics("Simple insert",
                               [{insert: 0, document: {_id: 0}}],
                               [{insert: collName, documents: [{_id: 0}]}],
                               {inserted: 1});

    metricChecker.checkMetrics("Update with pipeline",
                               [{update: 0, filter: {_id: 0}, updateMods: [{$set: {x: 1}}]}],
                               [{update: collName, updates: [{q: {_id: 0}, u: [{$set: {x: 1}}]}]}],
                               {updated: 1, updatePipeline: 1});

    assert.commandWorked(
        coll.insert({_id: 1, a: [{b: 5}, {b: 1}, {b: 2}]}, {writeConcern: {w: "majority"}}));
    metricChecker.checkMetrics(
        "Update with arrayFilters",
        [{
            update: 0,
            filter: {_id: 1},
            updateMods: {$set: {"a.$[i].b": 6}},
            arrayFilters: [{"i.b": 5}]
        }],
        [{
            update: collName,
            updates: [{q: {_id: 1}, u: {$set: {"a.$[i].b": 6}}, arrayFilters: [{"i.b": 5}]}]
        }],
        {updated: 1, updateArrayFilters: 1});

    metricChecker.checkMetrics("Simple delete",
                               [{delete: 0, filter: {_id: 0}}],
                               [{delete: collName, deletes: [{q: {_id: 0}, limit: 1}]}],
                               {deleted: 1});

    metricChecker.checkMetricsWithRetries("Simple insert with retry",
                                          [{insert: 0, document: {_id: 3}}],
                                          {
                                              insert: collName,
                                              documents: [{_id: 3}],
                                          },
                                          {inserted: 1, retriedInsert: retryCount - 1},
                                          session.getSessionId(),
                                          NumberLong(10));

    metricChecker.checkMetricsWithRetries(
        "Simple update with retry",
        [{
            update: 0,
            filter: {_id: 1},
            updateMods: {$set: {"a.$[i].b": 7}},
            arrayFilters: [{"i.b": 6}]
        }],
        {
            update: collName,
            updates: [{q: {_id: 1}, u: {$set: {"a.$[i].b": 7}}, arrayFilters: [{"i.b": 6}]}]
        },
        {
            updated: 1,
            updateArrayFilters: retryCount  // This is incremented even on a retry.
        },
        session.getSessionId(),
        NumberLong(11));

    // This one is set to have the 2 oneShard updates (each on a different shard) and 3 oneShard
    // inserts (each on a different shard). This means that the bulkWrite as a whole counts as 1 in
    // update.manyShards and 1 in insert.allShards.
    metricChecker.checkMetrics("Multiple operations",
                               [
                                   {insert: 0, document: {_id: 4, y: 2}},
                                   {update: 0, filter: {y: 2}, updateMods: {$set: {x: 2}}},
                                   {insert: 0, document: {y: 10}},
                                   {update: 0, filter: {y: 10}, updateMods: {$set: {x: 1}}},
                                   {insert: 0, document: {y: 300}},
                                   {delete: 0, filter: {_id: 4}}
                               ],
                               [
                                   {insert: collName, documents: [{_id: 4, y: 2}]},
                                   {update: collName, updates: [{q: {y: 2}, u: {$set: {x: 2}}}]},
                                   {insert: collName, documents: [{y: 10}]},
                                   {update: collName, updates: [{q: {y: 10}, u: {$set: {x: 2}}}]},
                                   {insert: collName, documents: [{y: 300}]},
                                   {delete: collName, deletes: [{q: {_id: 4}, limit: 1}]}
                               ],
                               {
                                   updated: 2,
                                   inserted: 3,
                                   deleted: 1,
                                   singleUpdateForBulkWrite: true,
                                   singleInsertForBulkWrite: true,
                                   insertShardField: bulkWrite ? "allShards" : "oneShard",
                                   updateShardField: bulkWrite ? "manyShards" : "oneShard"
                               });
    if (isMongos) {
        // Update modifying owning shard requires a transaction or retryable write, we do not want
        // actual retries here.
        metricChecker.retryCount = 1;
        metricChecker.checkMetricsWithRetries(
            "Update modifying owning shard",
            [
                {update: 0, filter: {y: 10}, updateMods: {$set: {y: 300}}},
            ],
            {update: collName, updates: [{q: {y: 10}, u: {$set: {y: 300}}}]},
            {updated: 1, updateShardField: "manyShards"},
            session.getSessionId(),
            NumberLong(12));
    }

    coll.drop();
}

{
    const testName = jsTestName();
    const replTest = new ReplSetTest({
        name: testName,
        nodes: [{}, {rsConfig: {priority: 0}}],
        nodeOptions: {
            setParameter: {
                // Required for serverStatus() to have opWriteConcernCounters.
                reportOpWriteConcernCountersInServerStatus: true
            }
        }
    });

    replTest.startSet();
    replTest.initiateWithHighElectionTimeout();

    const retryCount = 3;
    for (const bulkWrite of [false, true]) {
        runTest(false /* isMongos */, replTest, bulkWrite, retryCount);
    }

    replTest.stopSet();
}

{
    const st = new ShardingTest({mongos: 1, shards: 3, rs: {nodes: 1}, config: 1});

    const retryCount = 3;
    for (const bulkWrite of [false, true]) {
        runTest(true /* isMongos */, st, bulkWrite, retryCount);
    }

    st.stop();
}
