/**
 * Test the $currentDate expression.
 * @tags: [
 *  featureFlagCurrentDate,
 *  requires_fcv_81,
 *  uses_getmore_outside_of_transaction,
 *  # Some multitenancy passthrough suites do not enforce the failPoint this test requires.
 *  command_not_supported_in_serverless,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const coll = db.expression_currentDate;
assertDropCollection(db, "expression_currentDate");

// Validate the $currentDate expression returns the wall clock
// time when it is evaluated.
function basicTest() {
    const start = new Date().getTime();
    const N = 100;
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < N; i++) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());

    const pipeline = [
        // Evaluate a $group with a $currentDate expression.
        {$group: {_id: "$_id", time1: {$first: {$currentDate: {}}}}},
        // Evaluate a $group with another $currentDate expression.
        {$group: {_id: "$time1", time2: {$first: {$currentDate: {}}}}},
        {$project: {time1: "$_id", time2: 1}},
        {$project: {_id: 0}}
    ];

    const resultArray = coll.aggregate(pipeline).toArray();
    assert.gt(resultArray.length, 0);

    const tenMinutes = 1000 * 60 * 10;
    // Validate the time1 and time2 in each doc differs,
    // and the times are reasonable close to the start time of the query.
    for (const doc of resultArray) {
        assert.lte(Math.abs(doc.time1.getTime() - start), tenMinutes);
        assert.lte(Math.abs(doc.time2.getTime() - start), tenMinutes);
        assert.neq(doc.time1, doc.time2);
    }
}

let failPoints = [];
try {
    failPoints = FixtureHelpers.mapOnEachShardNode({
        db: db.getSiblingDB("admin"),
        func: (db) => configureFailPoint(db, "sleepBeforeCurrentDateEvaluation", {ms: 2}),
        primaryNodeOnly: false,
    });
    basicTest();
} finally {
    failPoints.forEach(failPoint => failPoint.off());
}
