/**
 * Tests that the createIndex command accepts a prepareUnique field and works accordingly.
 *
 * @tags: [assumes_no_implicit_collection_creation_after_drop]
 */
(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");

if (!FeatureFlagUtil.isEnabled(db, "CollModIndexUnique")) {
    jsTestLog('Skipping test because the collMod unique index feature flag is disabled.');
    return;
}

const collName_prefix = "index_prepareUnique";
let count = 0;
const coll1 = db.getCollection(collName_prefix + count++);
coll1.drop();

// The 'prepareUnique' option doesn't affect existing keys in the index.
assert.commandWorked(coll1.insert({_id: 0, a: 1}));
assert.commandWorked(coll1.insert({_id: 1, a: 1}));

// Starts rejecting new duplicate keys.
assert.commandWorked(coll1.createIndex({a: 1}, {prepareUnique: true}));

// Disallows creating another index on the same key with a different option.
assert.commandFailedWithCode(coll1.createIndex({a: 1}, {prepareUnique: false}),
                             ErrorCodes.IndexOptionsConflict);

// Checks the index is rejecting duplicates but accepting other keys.
assert.commandFailedWithCode(coll1.insert({_id: 2, a: 1}), ErrorCodes.DuplicateKey);
assert.commandWorked(coll1.insert({_id: 3, a: 2}));

// Checks that the prepareUnique field exists when getIndexes is called.
let indexesWithPrepareUnique = coll1.getIndexes().filter(function(doc) {
    return friendlyEqual(doc.prepareUnique, true);
});
assert.eq(1, indexesWithPrepareUnique.length);

// Removes the field and checks the index works as a regular index.
assert.commandWorked(coll1.runCommand(
    {collMod: coll1.getName(), index: {keyPattern: {a: 1}, prepareUnique: false}}));
assert.commandWorked(coll1.insert({_id: 2, a: 1}));

// Checks that the prepareUnique field is removed.
indexesWithPrepareUnique = coll1.getIndexes().filter(function(doc) {
    return friendlyEqual(doc.prepareUnique, true);
});
assert.eq(0, indexesWithPrepareUnique.length);

// Setting 'prepareUnique' on a unique index will be a no-op.
const coll2 = db.getCollection(collName_prefix + count++);
coll2.drop();

assert.commandWorked(coll2.createIndex({a: 1}, {unique: true}));
assert.commandWorked(
    db.runCommand({collMod: coll2.getName(), index: {keyPattern: {a: 1}, prepareUnique: true}}));
// Checks that the prepareUnique field does not exist when getIndexes is called.
indexesWithPrepareUnique = coll2.getIndexes().filter(function(doc) {
    return friendlyEqual(doc.prepareUnique, true);
});
assert.eq(0, indexesWithPrepareUnique.length);

// The 'prepareUnique' and 'unique' options cannot be both set when creating an index.
const coll3 = db.getCollection(collName_prefix + count++);
coll3.drop();

assert.commandFailedWithCode(coll3.createIndex({a: 1}, {unique: true, prepareUnique: true}),
                             ErrorCodes.CannotCreateIndex);

// The 'prepareUnique: false' can be passed along with the 'unique' option when creating an index.
const coll4 = db.getCollection(collName_prefix + count++);
coll4.drop();

assert.commandWorked(coll4.createIndex({a: 1}, {unique: true, prepareUnique: false}));
// Checks that the prepareUnique field does not exist when getIndexes is called.
indexesWithPrepareUnique = coll4.getIndexes().filter(function(doc) {
    return friendlyEqual(doc.prepareUnique, true);
});
assert.eq(0, indexesWithPrepareUnique.length);
})();
