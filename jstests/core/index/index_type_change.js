/**
 * Tests that replacing a document with an equivalent document with different types for the fields
 * will update the index entries associated with that document.
 * @tags: [
 *   requires_getmore,
 * ]
 */

import {isIndexOnly} from "jstests/libs/query/analyze_plan.js";

let coll = db.index_type_change;
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}));

assert.commandWorked(coll.insert({a: 2}));
assert.eq(1, coll.find({a: {$type: "double"}}).itcount());

let newVal = new NumberLong(2);
let res = coll.update({}, {a: newVal}); // Replacement update.
assert.commandWorked(res);
assert.eq(res.nMatched, 1);
assert.eq(res.nModified, 1);

// Make sure it actually changed the type.
assert.eq(1, coll.find({a: {$type: "long"}}).itcount());

// Now use a covered query to ensure the index entry has been updated.

// First make sure it's actually using a covered index scan.
let explain = coll.explain().find({a: 2}, {_id: 0, a: 1});
assert(isIndexOnly(db, explain));

let updated = coll.findOne({a: 2}, {_id: 0, a: 1});

assert(updated.a instanceof NumberLong, "Index entry did not change type");
