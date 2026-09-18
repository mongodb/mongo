/**
 * Test the handling of the _id field in various contexts in various DML operations
 *
 * @tags: [
 *   # Test uses commands that cannot be blindly retried.
 *   requires_non_retryable_commands,
 *   requires_non_retryable_writes,
 *   does_not_support_retryable_writes
 * ]
 */

db.dropDatabase();

/*
 * ==============================
 * insert() + MinValue, MaxValue, NaN, Infinity, -Infinity
 * ==============================
 */

assert.commandWorked(db.insert_one.insertOne({_id: MinKey}));
assert.commandWorked(db.insert_one.insertOne({_id: MaxKey}));
assert.commandWorked(db.insert_one.insertOne({_id: NaN}));
assert.commandWorked(db.insert_one.insertOne({_id: Infinity}));
assert.commandWorked(db.insert_one.insertOne({_id: -Infinity}));

assert.eq(db.insert_one.findOne({_id: MinKey})._id, {"$minKey": 1});
assert.eq(db.insert_one.findOne({_id: MaxKey})._id, {"$maxKey": 1});
assert.eq(db.insert_one.findOne({_id: NaN})._id, NaN);
assert.eq(db.insert_one.findOne({_id: Infinity})._id, Infinity);
assert.eq(db.insert_one.findOne({_id: -Infinity})._id, -Infinity);

/*
 * ==============================
 * insert() + Very large value in _id
 * ==============================
 */

const idLength = 1 * 1024 * 1024;
const idA = "A".repeat(idLength);

assert.commandWorked(db.insert_long.insertOne({_id: idA}));
assert.eq(db.insert_long.findOne({_id: idA})._id, idA);
