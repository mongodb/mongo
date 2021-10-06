/**
 * Tests inserting various _id values, duplicates, updates and secondary index lookups
 * on a collection clustered by _id.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_stepdowns,
 *   requires_fcv_51,
 *   requires_wiredtiger,
 *   tenant_migration_incompatible, #TODO: why is it incompatible?
 * ]
 */

(function() {
"use strict";

const clusteredIndexesEnabled = assert
                                    .commandWorked(db.getMongo().adminCommand(
                                        {getParameter: 1, featureFlagClusteredIndexes: 1}))
                                    .featureFlagClusteredIndexes.value;

if (!clusteredIndexesEnabled) {
    jsTestLog('Skipping test because the clustered indexes feature flag is disabled');
    return;
}

const collName = 'clustered_collection';
const coll = db[collName];

const lengths = [100, 1024, 1024 * 1024, 3 * 1024 * 1024];
coll.drop();

assert.commandWorked(
    db.createCollection(collName, {clusteredIndex: {key: {_id: 1}, unique: true}}));

// Expect that duplicates are rejected.
for (let len of lengths) {
    let id = 'x'.repeat(len);
    assert.commandWorked(coll.insert({_id: id}));
    assert.commandFailedWithCode(coll.insert({_id: id}), ErrorCodes.DuplicateKey);
    assert.eq(1, coll.find({_id: id}).itcount());
}

// Updates should work.
for (let len of lengths) {
    let id = 'x'.repeat(len);
    assert.commandWorked(coll.update({_id: id}, {a: len}));
    assert.eq(1, coll.find({_id: id}).itcount());
    assert.eq(len, coll.findOne({_id: id})['a']);
}

// This section is based on jstests/core/timeseries/clustered_index_crud.js with
// specific additions for general-purpose (non-timeseries) clustered collections
assert.commandWorked(coll.insert({_id: 0, a: 1}));
assert.commandWorked(coll.insert({_id: 1, a: 1}));
assert.eq(1, coll.find({_id: 0}).itcount());
assert.commandWorked(coll.insert({_id: "", a: 2}));
assert.eq(1, coll.find({_id: ""}).itcount());
assert.commandWorked(coll.insert({_id: NumberLong("9223372036854775807"), a: 3}));
assert.eq(1, coll.find({_id: NumberLong("9223372036854775807")}).itcount());
assert.commandWorked(coll.insert({_id: {a: 1, b: 1}, a: 4}));
assert.eq(1, coll.find({_id: {a: 1, b: 1}}).itcount());
assert.commandWorked(coll.insert({_id: {a: {b: 1}, c: 1}, a: 5}));
assert.commandWorked(coll.insert({_id: -1, a: 6}));
assert.eq(1, coll.find({_id: -1}).itcount());
assert.commandWorked(coll.insert({_id: "123456789012", a: 7}));
assert.eq(1, coll.find({_id: "123456789012"}).itcount());
assert.commandWorked(coll.insert({a: 8}));
assert.eq(1, coll.find({a: 8}).itcount());
assert.commandWorked(coll.insert({_id: null, a: 9}));
assert.eq(1, coll.find({_id: null}).itcount());
assert.commandWorked(coll.insert({_id: 'x'.repeat(99), a: 10}));
assert.commandWorked(coll.insert({}));

// Can build a secondary index with a 3MB RecordId doc.
assert.commandWorked(coll.createIndex({a: 1}));
// Can drop the secondary index
assert.commandWorked(coll.dropIndex({a: 1}));

// This key is too large.
assert.commandFailedWithCode(coll.insert({_id: 'x'.repeat(8 * 1024 * 1024), a: 11}), 5894900);

// Look up using the secondary index on {a: 1}
assert.commandWorked(coll.createIndex({a: 1}));
assert.eq(1, coll.find({a: null}).itcount());
assert.eq(0, coll.find({a: 0}).itcount());
assert.eq(2, coll.find({a: 1}).itcount());
assert.eq(1, coll.find({a: 2}).itcount());
assert.eq(1, coll.find({a: 8}).itcount());
assert.eq(1, coll.find({a: 9}).itcount());
assert.eq(null, coll.findOne({a: 9})['_id']);
assert.eq(1, coll.find({a: 10}).itcount());
assert.eq(99, coll.findOne({a: 10})['_id'].length);

// Secondary index lookups for documents with large RecordId's.
for (let len of lengths) {
    assert.eq(1, coll.find({a: len}).itcount());
    assert.eq(len, coll.findOne({a: len})['_id'].length);
}

// No support for numeric type differentiation.
assert.commandWorked(coll.insert({_id: 42.0}));
assert.commandFailedWithCode(coll.insert({_id: 42}), ErrorCodes.DuplicateKey);
assert.commandFailedWithCode(coll.insert({_id: NumberLong("42")}), ErrorCodes.DuplicateKey);
assert.eq(1, coll.find({_id: 42.0}).itcount());
assert.eq(1, coll.find({_id: 42}).itcount());
assert.eq(1, coll.find({_id: NumberLong("42")}).itcount());
})();
