(function() {
"use strict";

db.bson_compare_bug.drop();

// We want some BSON objects for this test. One convenient way to get that is to insert them
// into the database and then get them back through a query.
const coll = db.bson_compare_bug;
assert.commandWorked(coll.insert(
    [{_id: 1, obj: {val: [], _id: 1}}, {_id: 2, obj: {val: []}}, {_id: 3, obj: {_id: 1, val: []}}],
    {writeConcern: {w: "majority"}}));

// The $replaceRoot is so we can get back two results that have an "_id" field and one that
// doesn't. The first two results from this query are the same, except for that.
// res[0]: {val: [], _id: 1}
// res[1]: {val: []}
const res = coll.aggregate([{$sort: {_id: 1}}, {$replaceRoot: {newRoot: "$obj"}}]).toArray();
assert.eq(3, res.length);

// bsonBinaryEqual() should see that the BSON results from the query are not equal.
assert(!bsonBinaryEqual(res[0], res[1]));

// A magic trick: the shell represents the objects in res[0] and res[1] as JavaScript objects
// that internally store raw BSON data but also maintain JavaScript properties for each of their
// BSON fields. The BSON and JavaScript properties are kept in sync both ways. Reading the "val"
// property for the first time results in a call to BSONInfo::resolve(), which materializes the
// "val" BSON field as a JavaScript property. In this case, the resolve function also
// conservatively marks the object as "altered," because "val" is an array, and there's no way
// to observe modifications to it.
assert.eq(res[0].val, res[1].val);

// We repeat the BSON comparison, but this time, the objects are "altered," and bsonBinaryEqual
// needs to sync the JavaScript properties back into BSON. Before SERVER-39521, a bug in the
// conversion would ignore the "_id" field unless it was previously resolved, which would cause
// res[0] and res[1] to appear equal.
assert(!bsonBinaryEqual(res[0], res[1]));

// The bug that caused the "_id" field to get dropped in conversion involves code that is
// supposed to move the "_id" field to the front when converting a JavaScript object to BSON.
// This check ensures that "_id" is still getting moved to the front. The value of res[0] should
// now have changed so that both it and res[2] have their _id field first.
assert(bsonBinaryEqual(res[0], res[2]));
}());
