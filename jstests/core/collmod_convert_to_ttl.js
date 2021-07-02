/**
 * Basic js tests for the collMod command converting regular indexes to TTL indexes.
 *
 * @tags: [
 *  # Cannot implicitly shard accessed collections because of collection existing when none
 *  # expected.
 *  assumes_no_implicit_collection_creation_after_drop,
 *
 *  requires_non_retryable_commands,
 *  requires_ttl_index,
 *  multiversion_incompatible,
 * ]
 */

(function() {
'use strict';
let collName = "collmod_convert_to_ttl";
let coll = db.getCollection(collName);
coll.drop();
db.createCollection(collName);

function findTTL(key, expireAfterSeconds) {
    let all = coll.getIndexes();
    all = all.filter(function(z) {
        return z.expireAfterSeconds == expireAfterSeconds && friendlyEqual(z.key, key);
    });
    return all.length == 1;
}

// Creates a regular index and use collMod to convert it to a TTL index.
coll.dropIndex({a: 1});
coll.createIndex({a: 1});

// Tries to modify with a string 'expireAfterSeconds' value.
let res = db.runCommand(
    {"collMod": collName, "index": {"keyPattern": {a: 1}, "expireAfterSeconds": "100"}});
assert.commandFailedWithCode(res, ErrorCodes.TypeMismatch);

// Tries to modify with a negative 'expireAfterSeconds' value.
res =
    db.runCommand({"collMod": collName, "index": {"keyPattern": {a: 1}, "expireAfterSeconds": -1}});
assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);

// Tries to modify with an 'expireAfterSeconds' value too large.
res = db.runCommand(
    {"collMod": collName, "index": {"keyPattern": {a: 1}, "expireAfterSeconds": 10000000000000}});
assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);

// Successfully converts to a TTL index.
res = db.runCommand(
    {"collMod": collName, "index": {"keyPattern": {a: 1}, "expireAfterSeconds": 100}});
assert(findTTL({a: 1}, 100), "TTL index should be 100 now");

// Tries to convert a compound index to a TTL index.
coll.createIndex({a: 1, b: 1});
res = db.runCommand(
    {"collMod": collName, "index": {"keyPattern": {a: 1, b: 1}, "expireAfterSeconds": 100}});
assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);

// Tries to convert the '_id' index to a TTL index.
res = db.runCommand(
    {"collMod": collName, "index": {"keyPattern": {_id: 1}, "expireAfterSeconds": 100}});
assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);
})();