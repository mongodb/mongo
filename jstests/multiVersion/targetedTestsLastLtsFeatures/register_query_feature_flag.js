/**
 * In the case of accumulators or window functions registered with feature flags, this test verifies
 * that they throw a query feature not allowed error when their registered fcv is downgraded.
 *
 * For example: in this test, $concatArrays is registered as an accumulator and window function
 * under the feature flag gFeatureFlagArrayAccumulators which is gated under FCV 8.1. If the FCV is
 * downgraded to a version where the query feature is not allowed, then this should be caught at
 * runtime instead of parsetime.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const collName = jsTestName();

function runTest(db) {
    const coll = db[collName];
    assert(coll.drop());

    // Accumulator Case: The $concatArray accumulator is only allowed on FCV 8.1 and above. Given
    // that the current FCV is greater than 8.1, the operation should succeed.
    coll.aggregate([
        {$match: {_id: {$in: [0, 1, 2]}}},
        {$sort: {_id: 1}},
        {$group: {_id: null, allBooks: {$concatArrays: '$books'}}}
    ]);

    // Window Function Case: The $concatArray accumulator is only allowed on FCV 8.1 and above.
    // Given that the current FCV is greater than 8.1, the operation should succeed.
    coll.aggregate([{$setWindowFields: {output: {field: {$concatArrays: '$a'}}}}]);

    // Set the feature compatibility version to 8.0 (feature_flags::gFeatureFlagArrayAccumulators is
    // gated under FCV 8.1).
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "8.0", confirm: true}));

    // The following aggregate commands for the accumulator and window function cases should fail
    // with QueryFeatureNotAllowed.
    assert.throwsWithCode(() => coll.aggregate([
        {$match: {_id: {$in: [0, 1, 2]}}},
        {$sort: {_id: 1}},
        {$group: {_id: null, allBooks: {$concatArrays: '$books'}}}
    ]),
                          ErrorCodes.QueryFeatureNotAllowed);

    assert.throwsWithCode(
        () => coll.aggregate([{$setWindowFields: {output: {field: {$concatArrays: '$a'}}}}]),
        ErrorCodes.QueryFeatureNotAllowed);
}

(function testReplicaSet() {
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    const db = rst.getPrimary().getDB(jsTestName());
    try {
        runTest(db);
    } finally {
        rst.stopSet();
    }
})();
