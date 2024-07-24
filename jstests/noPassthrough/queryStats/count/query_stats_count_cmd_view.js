/**
 * This tests confirms that a count command on a view is implemented as an aggregation but stored as
 * a count command query shape.
 *
 * @tags: [featureFlagQueryStatsCountDistinct]
 */

import {
    assertAggregatedMetricsSingleExec,
    getLatestQueryStatsEntry,
    getQueryStats,
} from "jstests/libs/query_stats_utils.js";

const options = {
    setParameter: {internalQueryStatsRateLimit: -1},
};
const conn = MongoRunner.runMongod(options);
const viewsDB = conn.getDB("views_count");
assert.commandWorked(viewsDB.dropDatabase());

// Insert documents into a collection.
const collName = jsTestName();
const coll = viewsDB.getCollection(collName);
coll.insert({a: 1});
coll.insert({a: 2});
coll.insert({a: 3});
coll.insert({a: 4});

// Create a view on the data.
assert.commandWorked(viewsDB.runCommand({create: "identityView", viewOn: collName}));

const command = {
    count: "identityView",
    query: {a: 3}
};

// Check that query statistics are stored for a count command, not an aggregate command.
assert.commandWorked(viewsDB.runCommand(command));
const stats = getQueryStats(viewsDB.getMongo());
assert.eq(1, stats.length, stats);
const entry = getLatestQueryStatsEntry(conn, {collName: "identityView"});
assert.eq("count", entry.key.queryShape.command, entry);
assertAggregatedMetricsSingleExec(entry, {
    keysExamined: 0,
    docsExamined: 4,
    hasSortStage: false,
    usedDisk: false,
    fromMultiPlanner: false,
    fromPlanCache: false
});
// TODO SERVER-92771: Uncomment this test when docsReturned is properly set to 1.
// assertExpectedResults(entry,
//                       entry.key,
//                       /* expectedExecCount */ 1,
//                       /* expectedDocsReturnedSum */ 1,
//                       /* expectedDocsReturnedMax */ 1,
//                       /* expectedDocsReturnedMin */ 1,
//                       /* expectedDocsReturnedSumOfSq */ 1,
//                       /* getMores */ false);
MongoRunner.stopMongod(conn);
