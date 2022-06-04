/**
 * @tags: [
 *     requires_fastcount,
 *     operations_longer_than_stepdown_interval_in_txns,
 * ]
 */

(function() {
'use strict';

const collNamePrefix = 'insert1_';
let collCount = 0;

// _id field of inserted document should be generated if omitted.
let t = db.getCollection(collNamePrefix + collCount++);
t.drop();
let o = {a: 1};
assert.commandWorked(t.insert(o));
let doc = t.findOne();
assert.eq(1, doc.a);
assert(doc._id != null, tojson(doc));

// Confirm that default-constructed ObjectId in collection matches that in the insert request.
// Also, see OID definition in mongo/bson/oid.h.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
o = {
    a: 2,
    _id: new ObjectId()
};
let id = o._id;
assert.commandWorked(t.insert(o));
doc = t.findOne();
assert.eq(2, doc.a);
assert.eq(id, doc._id);

// Tests non-ObjectId type for _id field.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
o = {
    a: 3,
    _id: "asdf"
};
id = o._id;
assert.commandWorked(t.insert(o));
doc = t.findOne();
assert.eq(3, doc.a);
assert.eq(id, doc._id);

// Tests that the null value is acceptable for the _id field.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
o = {
    a: 4,
    _id: null
};
assert.commandWorked(t.insert(o));
doc = t.findOne();
assert.eq(4, doc.a);
assert.eq(null, doc._id, tojson(doc));

// Tests that _id field can be a number.
// This test contains 100,000 inserts which will get grouped together by the passthrough and
// create a very slow transaction in slow variants.
// See SERVER-53447 and operations_longer_than_stepdown_interval_in_txns tag.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
let toInsert = [];
const count = 100 * 1000;
for (let i = 0; i < count; ++i) {
    toInsert.push({_id: i, a: 5});
}
assert.commandWorked(t.insert(toInsert));
doc = t.findOne({_id: 1});
assert.eq(5, doc.a);
assert.eq(count, t.count(), "bad count");
})();
