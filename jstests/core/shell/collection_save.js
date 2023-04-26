/**
 * Tests 'db.collection.save()' mongo shell method.
 *
 */
(function() {
'use strict';

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const coll = testDB['collsave'];

let res;
// Saving a document without '_id' should trigger insert operation.
res = assert.commandWorked(coll.save({fruit: "cherry"}));
assert.eq(res.nInserted, 1);

// Saving a document with '_id' should trigger upsert operation.
res = assert.commandWorked(coll.save({_id: 0, fruit: "pear"}));
assert.eq(res.nUpserted, 1);
assert.eq(res.nModified, 0);
res = assert.commandWorked(coll.save({_id: 0, fruit: "pear updated"}));
assert.eq(res.nUpserted, 0);
assert.eq(res.nModified, 1);

// Verify forbidden cases,
assert.throws(() => coll.save(null), [], "saving null must throw an error");
assert.throws(() => coll.save(42), [], "saving a number must throw an error");
assert.throws(() => coll.save("The answer to life, the universe and everything"),
              [],
              "saving a string must throw an error");
assert.throws(() => coll.save([{"fruit": "mango"}, {"fruit": "orange"}]),
              [],
              "saving an array must throw an error");
})();
