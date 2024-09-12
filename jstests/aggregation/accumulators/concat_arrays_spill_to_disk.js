/**
 * Tests that the $concatArrays accumulator does not allow spilling to disk.
 * @tags: [featureFlagArrayAccumulators, requires_fcv_81, requires_collstats]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
const coll = db["concat_arrays_spill_to_disk"];
coll.drop();

const sharded = FixtureHelpers.isSharded(coll);

const memoryLimitMB = sharded ? 200 : 100;
const bigStr = Array(1024 * 1024 + 1).toString();  // 1MB of ','

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < memoryLimitMB + 10; i++) {
    bulk.insert({_id: i, bigArr: [i + bigStr], sortKey: i});
}
assert.commandWorked(bulk.execute());

assert.gt(coll.stats().size, memoryLimitMB * 1024 * 1024);

// Test accumulating all values into one array. On debug builds we will spill to disk for $group and
// so may hit the group error code before we hit ExceededMemoryLimit.
const pipeline =
    [{$sort: {sortKey: 1}}, {$group: {_id: null, bigArray: {$concatArrays: '$bigArr'}}}];
const expectedCodes =
    [ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed, ErrorCodes.ExceededMemoryLimit];

// Test that 'allowDiskUse: false' does indeed prevent spilling to disk.
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}, allowDiskUse: false}),
    expectedCodes);

// The $concatArrays accumulator does not support spilling to disk, so ensure that it will fail even
// when disk use is allowed.
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}, allowDiskUse: true}),
    expectedCodes);
