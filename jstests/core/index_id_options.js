/**
 * This test uses the local database to test non-standard index creation options on the _id field.
 * It uses the 'local' database because the scenarios in this test are not meant in a replicated
 * settings. This also means the test is not meant for sharding or retryable passthroughs.
 *
 * @tags: [
 *     assumes_standalone_mongod,
 * ]
 */

// Test creation of the _id index with various options:
// - _id indexes must have key pattern {_id: 1}.
// - The name of an _id index gets corrected to "_id_".
// - _id indexes cannot have any options other than "key", "name", "ns", "v", and "collation".
// - _id indexes must have the collection default collation.
// - Non-_id indexes cannot have the name "_id_".

(function() {
"use strict";

load("jstests/libs/get_index_helpers.js");

// Must use local db for testing because autoIndexId:false collections are not allowed in
// replicated databases.
var coll = db.getSiblingDB("local").index_id_options;

// _id indexes must have key pattern {_id: 1}.
coll.drop();
assert.commandWorked(coll.runCommand("create", {autoIndexId: false}));
assert.commandFailed(coll.createIndex({_id: -1}, {name: "_id_"}));

// The name of an _id index gets corrected to "_id_".
coll.drop();
assert.commandWorked(coll.runCommand("create", {autoIndexId: false}));
assert.commandWorked(coll.createIndex({_id: 1}, {name: "bad"}));
var spec = GetIndexHelpers.findByKeyPattern(coll.getIndexes(), {_id: 1});
assert.neq(null, spec, "_id index spec not found");
assert.eq("_id_", spec.name, tojson(spec));

// _id indexes cannot have any options other than "key", "name", "ns", "v", and "collation."
coll.drop();
assert.commandWorked(coll.runCommand("create", {autoIndexId: false}));
assert.commandFailed(coll.createIndex({_id: 1}, {name: "_id_", unique: true}));
assert.commandFailed(coll.createIndex({_id: 1}, {name: "_id_", sparse: false}));
assert.commandFailed(coll.createIndex({_id: 1}, {name: "_id_", partialFilterExpression: {a: 1}}));
assert.commandFailed(coll.createIndex({_id: 1}, {name: "_id_", expireAfterSeconds: 3600}));
assert.commandFailed(coll.createIndex({_id: 1}, {name: "_id_", background: false}));
assert.commandFailed(coll.createIndex({_id: 1}, {name: "_id_", unknown: true}));
assert.commandWorked(coll.createIndex(
    {_id: 1}, {name: "_id_", ns: coll.getFullName(), v: 2, collation: {locale: "simple"}}));

// _id indexes must have the collection default collation.
coll.drop();
assert.commandWorked(coll.runCommand("create", {autoIndexId: false}));
assert.commandFailed(coll.createIndex({_id: 1}, {name: "_id_", collation: {locale: "en_US"}}));
assert.commandWorked(coll.createIndex({_id: 1}, {name: "_id_", collation: {locale: "simple"}}));

coll.drop();
assert.commandWorked(coll.runCommand("create", {autoIndexId: false}));
assert.commandWorked(coll.createIndex({_id: 1}, {name: "_id_"}));

coll.drop();
assert.commandWorked(coll.runCommand("create", {autoIndexId: false, collation: {locale: "en_US"}}));
assert.commandFailed(coll.createIndex({_id: 1}, {name: "_id_", collation: {locale: "simple"}}));
assert.commandFailed(coll.createIndex({_id: 1}, {name: "_id_", collation: {locale: "fr_CA"}}));
assert.commandWorked(
    coll.createIndex({_id: 1}, {name: "_id_", collation: {locale: "en_US", strength: 3}}));

coll.drop();
assert.commandWorked(coll.runCommand("create", {autoIndexId: false, collation: {locale: "en_US"}}));
assert.commandWorked(coll.createIndex({_id: 1}, {name: "_id_"}));
spec = GetIndexHelpers.findByName(coll.getIndexes(), "_id_");
assert.neq(null, spec, "_id index spec not found");
assert.eq("en_US", spec.collation.locale, tojson(spec));

// Non-_id indexes cannot have the name "_id_".
coll.drop();
assert.commandWorked(coll.runCommand("create", {autoIndexId: false}));
assert.commandFailed(coll.createIndex({_id: "hashed"}, {name: "_id_"}));
assert.commandFailed(coll.createIndex({a: 1}, {name: "_id_"}));

// Non-_id indexes can have direction values outside the range for the integer type.
coll.drop();
assert.commandWorked(coll.insert({_id: 0}));
assert.commandWorked(coll.createIndex({_id: Number.MAX_VALUE}));
assert.commandWorked(coll.createIndex({_id: -Number.MAX_VALUE}));
})();
