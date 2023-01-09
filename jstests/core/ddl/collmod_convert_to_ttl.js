/**
 * Basic js tests for the collMod command converting regular indexes to TTL indexes.
 *
 * @tags: [
 *  # Cannot implicitly shard accessed collections because of collection existing when none
 *  # expected.
 *  assumes_no_implicit_collection_creation_after_drop,
 *  requires_non_retryable_commands,
 *  requires_ttl_index,
 *  requires_fcv_51,
 * ]
 */

(function() {
'use strict';
const collName = "collmod_convert_to_ttl";
const coll = db.getCollection(collName);
coll.drop();
db.createCollection(collName);

function findTTL(key, expireAfterSeconds) {
    const all = coll.getIndexes().filter(function(z) {
        return z.expireAfterSeconds == expireAfterSeconds && friendlyEqual(z.key, key);
    });
    return all.length == 1;
}

// Creates a regular index and use collMod to convert it to a TTL index.
coll.createIndex({a: 1});

// Tries to modify with a string 'expireAfterSeconds' value.
assert.commandFailedWithCode(
    db.runCommand(
        {"collMod": collName, "index": {"keyPattern": {a: 1}, "expireAfterSeconds": "100"}}),
    ErrorCodes.TypeMismatch);

// Tries to modify with a negative 'expireAfterSeconds' value.
assert.commandFailedWithCode(
    db.runCommand({"collMod": collName, "index": {"keyPattern": {a: 1}, "expireAfterSeconds": -1}}),
    ErrorCodes.InvalidOptions);

// Successfully converts to a TTL index.
assert.commandWorked(db.runCommand(
    {"collMod": collName, "index": {"keyPattern": {a: 1}, "expireAfterSeconds": 100}}));
assert(findTTL({a: 1}, 100), "TTL index should be 100 now");

// Tries to convert a compound index to a TTL index.
coll.createIndex({a: 1, b: 1});
assert.commandFailedWithCode(
    db.runCommand(
        {"collMod": collName, "index": {"keyPattern": {a: 1, b: 1}, "expireAfterSeconds": 100}}),
    ErrorCodes.InvalidOptions);

// Tries to convert the '_id' index to a TTL index.
assert.commandFailedWithCode(
    db.runCommand(
        {"collMod": collName, "index": {"keyPattern": {_id: 1}, "expireAfterSeconds": 100}}),
    ErrorCodes.InvalidOptions);

// Tries to convert an index on a capped collection to a TTL index.
const collCapped = db.getCollection(collName + "_capped");
collCapped.drop();
db.createCollection(collCapped.getName(), {capped: true, size: 1024 * 1024});
collCapped.createIndex({a: 1});
assert.commandFailedWithCode(db.runCommand({
    "collMod": collCapped.getName(),
    "index": {"keyPattern": {a: 1}, "expireAfterSeconds": 100}
}),
                             ErrorCodes.InvalidOptions);
})();
