/**
 * Tests that when "searchScore" and "vectorSearchScore" metadata fields are referenced
 * in a _valid_ way, no warning log lines are printed.
 *
 * The test in noPassthrough/query/search_score_meta_validation_warning_log.js is responsible
 * for testing that the warning log lines _are_ printed when the metadata is referenced incorrectly.
 *
 * The testing must be separate since this test requires a valid $search/$vectorSearch query run on
 * mongot, but the other test requires noPassthrough to manually check the correct node's logs. We
 * do not have the infrastructure to run a noPassthrough test on a real mongot.
 *
 * @tags: [requires_fcv_82]
 */
import {iterateMatchingLogLines} from "jstests/libs/log.js";

const warningLogMessage =
    "The query attempts to retrieve metadata at a place in the pipeline where that metadata type is not available";

const dbName = "test";
const collName = "search_score_meta_validation_e2e";
const data = [
    {a: 1, b: "foo", score: 10},
    {a: 2, b: "bar", score: 20},
    {a: 3, b: "baz", score: 30},
    {a: 4, b: "qux", score: 40},
    {a: 5, b: "quux", score: 50},
];

function checkNoWarningLogs(db) {
    const globalLogs = db.adminCommand({getLog: 'global'});
    const matchingLogLines = [...iterateMatchingLogLines(globalLogs.log, {msg: warningLogMessage})];
    assert.eq(matchingLogLines.length, 0, matchingLogLines);
}

const testDB = db.getSiblingDB(dbName);
const coll = testDB.getCollection(collName);

coll.drop();
assert.commandWorked(coll.insertMany(data));

// Clear logs before running.
assert.commandWorked(testDB.adminCommand({clearLog: "global"}));

function runValidQueries() {
    // Valid $meta: "searchScore" usage in $project.
    assert.commandWorked(testDB.runCommand({
        aggregate: collName,
        pipeline: [
            {$search: {text: {query: "foo", path: "b"}}},
            {$project: {_id: 0, b: 1, score: {$meta: "searchScore"}}}
        ],
        cursor: {}
    }));

    // Valid $meta: "vectorSearchScore" usage in $project.
    assert.commandWorked(testDB.runCommand({
        aggregate: collName,
        pipeline: [
            {
                $vectorSearch: {
                    queryVector: [0.1, 0.2, 0.3],
                    path: "embedding",
                    numCandidates: 5,
                    index: "vector_index",
                    limit: 5
                }
            },
            {$project: {_id: 0, b: 1, score: {$meta: "vectorSearchScore"}}}
        ],
        cursor: {}
    }));
}

runValidQueries();

// Check that no warning logs are present.
checkNoWarningLogs(testDB);

// Check that we can run valid queries multiple times without generating warning logs.
for (let i = 0; i < 50; i++) {
    runValidQueries();
}

// Check that still no warning logs are present.
checkNoWarningLogs(testDB);
