/**
 * Tests for invalid usages of $unionWith, or invalid stages within the $unionWith sub-pipeline.
 * @tags: [
 *  # Some stages we're checking are only supported with a single read concern.
 *  assumes_read_concern_unchanged
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For isReplSet() and isSharded().

const baseColl = db["base"];
baseColl.drop();
const unionColl = db["union"];
unionColl.drop();

// Ensure the base collection exists.
assert.commandWorked(baseColl.insert({a: 1}));

// Disallowed within an update pipeline.
assert.commandFailedWithCode(baseColl.update({a: 1}, [{$unionWith: unionColl.getName()}]),
                             ErrorCodes.InvalidOptions);

function assertFailsWithCode(pipeline, errCode) {
    assert.commandFailedWithCode(db.runCommand({
        aggregate: baseColl.getName(),
        pipeline: pipeline,
        cursor: {},
    }),
                                 errCode);
}

// Change streams are only supported against a replica set.
if (FixtureHelpers.isReplSet(db) || FixtureHelpers.isSharded(baseColl)) {
    // Disallowed alongside a $changeStream.
    assertFailsWithCode([{$changeStream: {}}, {$unionWith: unionColl.getName()}],
                        ErrorCodes.IllegalOperation);

    // Likewise, $changeStream is disallowed within a $unionWith sub-pipeline.
    assertFailsWithCode(
        [{$unionWith: {coll: unionColl.getName(), pipeline: [{$changeStream: {}}]}}], 31441);

    assert.commandFailedWithCode(db.runCommand({
        aggregate: 1,
        pipeline: [{$changeStream: {}}, {$unionWith: unionColl.getName()}],
        cursor: {},
    }),
                                 ErrorCodes.IllegalOperation);
}

// $unionWith sub-pipeline cannot contain stages which write data ($merge, $out).
let subPipe = [{$out: "some_out_coll"}];
assertFailsWithCode([{$unionWith: {coll: unionColl.getName(), pipeline: subPipe}}], 31441);

subPipe = [{
    $merge: {
        into: {db: db.getName(), coll: "some_merge_coll"},
        whenMatched: "replace",
        whenNotMatched: "fail"
    }
}];
assertFailsWithCode([{$unionWith: {coll: unionColl.getName(), pipeline: subPipe}}], 31441);

// Test that collection-less stages are not allowed within the $unionWith sub-pipeline.
subPipe = [{$listCachedAndActiveUsers: {}}];
assertFailsWithCode([{$unionWith: {coll: unionColl.getName(), pipeline: subPipe}}],
                    ErrorCodes.InvalidNamespace);

subPipe = [{$listLocalSessions: {}}];
assertFailsWithCode([{$unionWith: {coll: unionColl.getName(), pipeline: subPipe}}],
                    ErrorCodes.InvalidNamespace);

if (FixtureHelpers.isSharded(baseColl)) {
    subPipe = [{$currentOp: {localOps: true}}];
    assertFailsWithCode([{$unionWith: {coll: unionColl.getName(), pipeline: subPipe}}],
                        ErrorCodes.InvalidNamespace);
}
})();
