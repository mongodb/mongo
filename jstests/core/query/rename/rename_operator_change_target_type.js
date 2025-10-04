/**
 * Test that a rename that overwrites its destination with an equivalent value of a different type
 * updates the type of the destination (SERVER-32109).
 *
 * @tags: [
 *   requires_non_retryable_writes,
 *   # update with multi:false is not supported on sharded collection
 *   assumes_unsharded_collection,
 *   requires_getmore,
 * ]
 */

let coll = db.rename_operator_change_target_type;
coll.drop();

assert.commandWorked(coll.insert({to: NumberLong(100), from: 100}));
assert.commandWorked(coll.update({}, {$rename: {from: "to"}}));

let aggResult = coll.aggregate([{$project: {toType: {$type: "$to"}}}]).toArray();
assert.eq(aggResult.length, 1);
assert.eq(aggResult[0].toType, "double", "Incorrect type resulting from $rename");
