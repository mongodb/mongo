/**
 * Tests passing sort to the updateOne command in a sharded environment:
 *   - A bad argument to the sort option should raise an error.
 *   - Sort will only have functionality in updateOne, not update or updateMany
 *
 * @tags: [
 *   requires_multi_updates,
 *   requires_non_retryable_writes,
 *   requires_fcv_81,
 * ]
 */

const coll = db[jsTestName()];

/**
 * Tests for updateOne using the CRUD API
 */

//
// updateOne basic functionality
//
coll.drop();
for (let i = 0; i <= 10; i++) {
    assert.commandWorked(coll.insert({name: "Jane", ranking: i}));
}

// Call updateOne with no sort object - any matching document can be chosen.
let res = assert.commandWorked(coll.updateOne({name: "Jane"}, {$set: {ranking: 11}}));
assert.eq(1, res.modifiedCount);
assert.eq(coll.find({name: "Jane", ranking: 11}).count(), 1); // new value

// Call updateOne with empty sort object - any matching document can be chosen.
res = assert.commandWorked(coll.updateOne({name: "Jane"}, {$set: {ranking: 12}}, {sort: {}}));
assert.eq(1, res.modifiedCount);
assert.eq(coll.find({name: "Jane", ranking: 12}).count(), 1); // new value

assert(coll.drop());

for (let i = 0; i <= 10; i++) {
    assert.commandWorked(coll.insert({name: "Jane", ranking: i}));
}

// Sort on ranking in ascending order.
res = assert.commandWorked(coll.updateOne({name: "Jane"}, {$set: {ranking: 6}}, {sort: {ranking: 1}}));
assert.eq(1, res.modifiedCount);
assert.eq(coll.find({name: "Jane", ranking: 0}).count(), 0); // old value
assert.eq(coll.find({name: "Jane", ranking: 6}).count(), 2); // new value

// Sort on ranking in descending order.
res = assert.commandWorked(coll.updateOne({name: "Jane"}, {$set: {ranking: 100}}, {sort: {ranking: -1}}));
assert.eq(1, res.modifiedCount);
assert.eq(coll.find({name: "Jane", ranking: 10}).count(), 0); // old value
assert.eq(coll.find({name: "Jane", ranking: 100}).count(), 1); // new value

// Sort while calling $set on multiple fields
res = assert.commandWorked(coll.updateOne({name: "Jane"}, {$set: {name: "Jean", ranking: 101}}, {sort: {ranking: -1}}));
assert.eq(1, res.modifiedCount);
assert.eq(coll.find({name: "Jane", ranking: 100}).count(), 0); // old value
assert.eq(coll.find({name: "Jean", ranking: 101}).count(), 1); // new value

// Sort with inequality filter
res = assert.commandWorked(coll.updateOne({ranking: {$lt: 10}}, {$set: {ranking: 10}}, {sort: {ranking: -1}}));
assert.eq(1, res.modifiedCount);
assert.eq(coll.find({name: "Jane", ranking: 9}).count(), 0); // old value
assert.eq(coll.find({name: "Jane", ranking: 10}).count(), 1); // new value

// updateOne with sort and upsert: true will insert documents when there is no match.
res = assert.commandWorked(coll.updateOne({name: "Lucy"}, {$set: {ranking: 11}}, {sort: {ranking: 1}, upsert: true}));
assert.neq(res.upsertedId, null);
assert.eq(coll.find({name: "Lucy"}).count(), 1);

// updateOne with sort and upsert: true will modify document and respect sort when there is a match.
assert.commandWorked(coll.insert({name: "Lucy", ranking: 13}));
res = assert.commandWorked(coll.updateOne({name: "Lucy"}, {$set: {ranking: 12}}, {sort: {ranking: -1}, upsert: true}));
assert.eq(res.upsertedId, null);
assert.eq(coll.find({name: "Lucy", ranking: 13}).count(), 0);
assert.eq(coll.find({name: "Lucy", ranking: 12}).count(), 1);

//
// updateOne with incorrectly formatted sorts
//

assert.throwsWithCode(
    () => coll.updateOne({name: "Jane"}, {$set: {ranking: 100}}, {sort: true}),
    ErrorCodes.TypeMismatch,
);
assert.throwsWithCode(() => coll.updateOne({name: "Jane"}, {$set: {ranking: 100}}, {sort: {ranking: 50}}), 15975); // $sort key ordering must be 1 (for ascending) or -1 (for descending)

//
// updateOne with multiple documents matching the query and sort only updates one document.
//
assert(coll.drop());

for (let i = 0; i <= 10; i++) {
    assert.commandWorked(coll.insert({name: "Lucy", ranking: i}));
    assert.commandWorked(coll.insert({name: "Lucy", ranking: i}));
}

res = assert.commandWorked(coll.updateOne({name: "Lucy"}, {$set: {ranking: -1}}, {sort: {ranking: 1}}));
assert.eq(1, res.modifiedCount);
assert.eq(coll.find({name: "Lucy", ranking: 0}).count(), 1); // old value
assert.eq(coll.find({name: "Lucy", ranking: -1}).count(), 1); // new value

res = assert.commandWorked(coll.updateOne({name: "Lucy"}, {$set: {ranking: 11}}, {sort: {ranking: -1}}));
assert.eq(1, res.modifiedCount);
assert.eq(coll.find({name: "Lucy", ranking: 10}).count(), 1); // old value
assert.eq(coll.find({name: "Lucy", ranking: 11}).count(), 1); // new value

//
// updateOne with a long-running sort on a large collection
//

assert(coll.drop());

for (let i = 0; i <= 1000; i++) {
    assert.commandWorked(coll.insert({name: "Jane", ranking: i}));
    assert.commandWorked(coll.insert({name: "Lucy", ranking: i}));
}

res = assert.commandWorked(coll.updateOne({name: "Lucy"}, {$set: {ranking: -1}}, {sort: {ranking: -1}}));
assert.eq(1, res.modifiedCount);
assert.eq(coll.find({name: "Lucy", ranking: 1000}).count(), 0); // old value
assert.eq(coll.find({name: "Lucy", ranking: -1}).count(), 1); // new value

//
// updateOne on an indexed collection
//

assert(coll.drop());
assert.commandWorked(coll.createIndex({ranking: 1}));

for (let i = 0; i <= 10; i++) {
    assert.commandWorked(coll.insert({name: "Jane", ranking: i}));
}

// Index is in the same order as the sort specification.
res = assert.commandWorked(coll.updateOne({name: "Jane"}, {$set: {ranking: -1}}, {sort: {ranking: 1}}));
assert.eq(1, res.modifiedCount);
assert.eq(coll.find({name: "Jane", ranking: 0}).count(), 0); // old value
assert.eq(coll.find({name: "Jane", ranking: -1}).count(), 1); // new value

// Index is in the opposite order as the sort specification.
res = assert.commandWorked(coll.updateOne({name: "Jane"}, {$set: {ranking: -1}}, {sort: {ranking: -1}}));
assert.eq(1, res.modifiedCount);
assert.eq(coll.find({name: "Jane", ranking: 10}).count(), 0); // old value
assert.eq(coll.find({name: "Jane", ranking: -1}).count(), 2); // new value

//
// updateOne with collation
// Note: For sharded collections with a default collation, {locale: "simple"} must be specified in
// the collation field. Implicit collations in the collection that don't meet that specification
// will not be respected if a sort is provided.
//

assert(coll.drop());
const caseInsensitive = {
    collation: {locale: "en_US", strength: 2},
};
const caseSensitive = {
    collation: {locale: "en_US", strength: 3},
};

assert.commandWorked(
    coll.insertMany([
        {name: "Bella", age: 12},
        {name: "bella", age: 12},
        {name: "bella", age: 9},
        {name: "Candice", age: 12},
        {name: "candice", age: 12},
    ]),
);

// Sort + collation provided through command, case sensitive
// Ascending sort
res = assert.commandWorked(
    coll.updateOne({age: 12}, {$set: {age: 13}}, {sort: {name: 1}, collation: caseSensitive.collation}),
);
assert.eq(1, res.modifiedCount);
assert.eq(coll.find({name: "bella", age: 12}).count(), 0); // old value
assert.eq(coll.find({name: "bella", age: 13}).count(), 1); // new value
assert.eq(coll.find({name: "Bella", age: 12}).count(), 1); // "Bella" doc unmodified

// Descending sort
res = assert.commandWorked(
    coll.updateOne({age: 12}, {$set: {age: 13}}, {sort: {name: -1}, collation: caseSensitive.collation}),
);
assert.eq(1, res.modifiedCount);
assert.eq(coll.find({name: "Candice", age: 12}).count(), 0); // old value
assert.eq(coll.find({name: "Candice", age: 13}).count(), 1); // new value

// Sort + collation provided through command, case insensitive
res = assert.commandWorked(
    coll.updateOne({name: "bella"}, {$set: {age: 14}}, {sort: {name: 1}, collation: caseInsensitive.collation}),
);
assert.eq(1, res.modifiedCount);
assert.eq(coll.find({name: "Bella"}).collation(caseInsensitive.collation).count(), 3);
assert.eq(coll.find({name: "Bella", age: 14}).collation(caseInsensitive.collation).count(), 1); // new value

/**
 * Test that sort is respected for pipeline-style bulk update.
 */

assert(coll.drop());

for (let i = 0; i <= 10; i++) {
    assert.commandWorked(coll.insert({name: "Jane", ranking: i}));
}

assert.commandWorked(
    coll.bulkWrite([{updateOne: {filter: {name: "Jane"}, update: [{$set: {ranking: 100}}], sort: {ranking: -1}}}]),
);
assert.eq(1, res.modifiedCount);
assert.eq(coll.find({name: "Jane", ranking: 10}).count(), 0); // old value
assert.eq(coll.find({name: "Jane", ranking: 100}).count(), 1); // new value

assert.commandWorked(
    coll.bulkWrite([{updateOne: {filter: {name: "Jane"}, update: [{$set: {ranking: 100}}], sort: {ranking: 1}}}]),
);
assert.eq(1, res.modifiedCount);
assert.eq(coll.find({name: "Jane", ranking: 0}).count(), 0); // old value
assert.eq(coll.find({name: "Jane", ranking: 100}).count(), 2); // new value

/**
 * Test that update and updateMany don't have sort support.
 */

assert(coll.drop());

for (let i = 0; i <= 10; i++) {
    assert.commandWorked(coll.insert({name: "Jane", ranking: i}));
}

// Call update with no sort object.
res = assert.commandWorked(coll.update({name: "Jane"}, {$set: {ranking: 11}}));
assert.eq(1, res.nModified);
assert.eq(coll.find({name: "Jane", ranking: 11}).count(), 1); // new value

// Calling update with sort will error. The error is thrown from the shell helper for _parsedUpdate
// in collection.js.
assert.throws(() => coll.update({name: "Jane"}, {$set: {ranking: 5}}, {sort: {}}));

// Calling updateMany with sort will error. The error is thrown from the shell helper for updateMany
// in crud_api.js.
assert(coll.drop());

for (let i = 0; i <= 10; i++) {
    assert.commandWorked(coll.insert({name: "Jane", ranking: 1}));
    assert.commandWorked(coll.insert({name: "Lucy", ranking: 1}));
}

assert.throws(() => coll.updateMany({ranking: 1}, {$set: {ranking: 100}}, {sort: {name: 1}}));

// Calling updateMany with sort through bulk api will error. The error is thrown from the shell
// helper for bulkWrite.updateMany in crud_api.js.
assert.throws(() =>
    coll.bulkWrite([{updateMany: {filter: {ranking: 1}, update: {$set: {ranking: 100}}, sort: {name: 1}}}]),
);

// Calling a pipeline-style update with sort and multi=true will error.
let updateCmd = {
    update: coll.getName(),
    updates: [{q: {ranking: 1}, u: {$set: {ranking: 100}}, multi: true, sort: {a: -1}}],
};
assert.commandFailedWithCode(db.runCommand(updateCmd), ErrorCodes.FailedToParse);
