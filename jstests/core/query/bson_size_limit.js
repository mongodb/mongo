/**
 * Test that aggregate, find, and distinct commands can return documents that are 16 MB (the user
 * BSON size limit) or larger (if applicable).
 *
 * Note that that this latter case is not a goal/officially supported behavior, but it is allowed
 * today and we want to make sure nothing chokes when a document that is larger than 16MB is
 * returned.
 *
 * @tags: [
 *     # These view reads are flaky in suites that enable transactions.
 *     does_not_support_transactions,
 *     # Overflows WT cache on in-memory variants.
 *     requires_persistence,
 * ]
 */
const collName = jsTestName();
const coll = db[collName];
coll.drop();

// The max size limit for a stored document is 16 MB.
const bsonMaxSizeLimit = 16 * 1024 * 1024;

//
// Test that find and aggregate commands can return a document that is exactly 16 MB.
//

const maxSizeDoc = {
    _id: 1,
    x: Array(bsonMaxSizeLimit - 25).join("a")
};
assert.eq(Object.bsonsize(maxSizeDoc), bsonMaxSizeLimit);
assert.commandWorked(coll.insert(maxSizeDoc));

// Return a document that is just short of 16MB.
assert.eq(coll.aggregate().itcount(), 1);
assert.eq(coll.find().itcount(), 1);

// Find and distinct go through the aggregation path when run on a view, so anything that succeeds
// on an aggregate should succeed here.
const identityViewName = collName + "_identity_view";
db.createView(identityViewName, collName, []);

assert.eq(db[identityViewName].find().itcount(), 1);
assert.eq(db[identityViewName].distinct("x").length, 1);

//
// Test that applicable read commands can return a document that is just over 16 MB.
//

// Insert another document. We'll accumulate both documents into one large document that surpasses
// the size limit.
assert.commandWorked(coll.insert({_id: 2, x: Array(1024).join("b")}));

// Return a document that is just over 16MB (by ~1KB).
const results = coll.aggregate({$group: {_id: null, xlField: {$addToSet: "$x"}}}).toArray();
assert.eq(results.length, 1);
assert.gt(Object.bsonsize(results[0]), bsonMaxSizeLimit);

// Distinct strictly enforces the size limit when aggregating all distinct values.
assert.throwsWithCode(() => coll.distinct("x"), 17217);

const xlFieldViewName = collName + "_xl_field_view";
db.createView(xlFieldViewName, collName, [{$group: {_id: null, xlField: {$addToSet: "$x"}}}]);

assert.eq(db[xlFieldViewName].find().itcount(), 1);
// Return two documents because distinct traverses arrays.
assert.eq(db[xlFieldViewName].distinct("xlField").length, 2);

//
// Test distinct size limit.
//

// The normal distinct path actually doesn't allow returning 16MB values. Do one final pass
// verifying that it gets close.
coll.drop();
assert.commandWorked(coll.insert({_id: 1, x: Array(bsonMaxSizeLimit - 4096 - 100).join("a")}));

assert.eq(coll.distinct("x").length, 1);
