/**
 * Test that bulkWrite emit the same metrics that corresponding calls to update, delete and insert.
 *
 * @tags: [featureFlagBulkWriteCommand] // TODO SERVER-52419: Remove this tag.
 */

import {BulkWriteMetricChecker} from "jstests/libs/bulk_write_utils.js";

function runTest(replTest, bulkWrite) {
    print("Running test with bulkWrite = " + bulkWrite);
    const dbName = "testDB";
    const collName = "testColl";
    const namespace = `${dbName}.${collName}`;
    const primary = replTest.getPrimary();
    const session = primary.startSession();
    const testDB = session.getDatabase(dbName);
    const coll = testDB[collName];

    // Simplifies implementation of checkBulkWriteMetrics:
    // totals["testDB.testColl"] will not be undefined on first top call below.
    coll.insert({_id: 99});

    const metricChecker = new BulkWriteMetricChecker(testDB, namespace, bulkWrite, false /*fle*/);

    metricChecker.checkMetrics("Simple insert.",
                               [{insert: 0, document: {_id: 0}}],
                               [{insert: collName, documents: [{_id: 0}]}],
                               {inserted: 1});

    metricChecker.checkMetrics("Update with pipeline.",
                               [{update: 0, filter: {_id: 0}, updateMods: [{$set: {x: 1}}]}],
                               [{update: collName, updates: [{q: {_id: 0}, u: [{$set: {x: 1}}]}]}],
                               {updated: 1, updatePipeline: 1, keysExamined: 1});

    assert.commandWorked(
        coll.insert({_id: 1, a: [{b: 5}, {b: 1}, {b: 2}]}, {writeConcern: {w: "majority"}}));
    metricChecker.checkMetrics(
        "Update with arrayFilters.",
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
        {updated: 1, updateArrayFilters: 1, keysExamined: 1});

    metricChecker.checkMetrics("Simple delete.",
                               [{delete: 0, filter: {_id: 0}}],
                               [{delete: collName, deletes: [{q: {_id: 0}, limit: 1}]}],
                               {deleted: 1, keysExamined: 1});

    metricChecker.checkMetricsWithRetries(
        "Simple insert with retry.",
        [{insert: 0, document: {_id: 3}}],
        {
            insert: collName,
            documents: [{_id: 3}],
        },
        {inserted: 1, retriedInsert: 1, retriedCommandsCount: 1, retriedStatementsCount: 1},
        session.getSessionId(),
        NumberLong(10));

    metricChecker.checkMetricsWithRetries(
        "Simple update with retry.",
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
            retriedCommandsCount: 1,
            retriedStatementsCount: 1,
            updateArrayFilters: 2,  // This is incremented even on a retry.
            keysExamined: 1
        },
        session.getSessionId(),
        NumberLong(11));

    metricChecker.checkMetrics("Multiple operations.",
                               [
                                   {insert: 0, document: {_id: 4}},
                                   {update: 0, filter: {_id: 4}, updateMods: {$set: {x: 2}}},
                                   {insert: 0, document: {_id: 5}},
                                   {update: 0, filter: {_id: 5}, updateMods: {$set: {x: 1}}},
                                   {insert: 0, document: {_id: 6}},
                                   {delete: 0, filter: {_id: 4}}
                               ],
                               [
                                   {insert: collName, documents: [{_id: 4}]},
                                   {update: collName, updates: [{q: {_id: 4}, u: {x: 2}}]},
                                   {insert: collName, documents: [{_id: 5}]},
                                   {update: collName, updates: [{q: {_id: 5}, u: {x: 1}}]},
                                   {insert: collName, documents: [{_id: 6}]},
                                   {delete: collName, deletes: [{q: {_id: 4}, limit: 1}]}
                               ],
                               {updated: 2, inserted: 3, deleted: 1, keysExamined: 3});

    coll.drop();
}

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

for (const bulkWrite of [false, true]) {
    runTest(replTest, bulkWrite);
}

replTest.stopSet();
