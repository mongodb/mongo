/**
 * Basic js tests for the collMod command.
 * Test modifying TTL indexes.
 *
 * @tags: [
 *  # Cannot implicitly shard accessed collections because of collection existing when none
 *  # expected.
 *  assumes_no_implicit_collection_creation_after_drop,
 *
 *  requires_non_retryable_commands,
 *  requires_ttl_index,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

function debug(x) {
    // printjson( x );
}

let coll = "collModTest";
let t = db.getCollection(coll);
t.drop();

let isMongos = "isdbgrid" == db.runCommand("hello").msg;

db.createCollection(coll);

function findTTL(key, expireAfterSeconds) {
    let all = t.getIndexes();
    all = all.filter(function (z) {
        return z.expireAfterSeconds == expireAfterSeconds && friendlyEqual(z.key, key);
    });
    return all.length == 1;
}

function findTTLByName(name, expireAfterSeconds) {
    let all = t.getIndexes();
    all = all.filter(function (z) {
        return z.expireAfterSeconds == expireAfterSeconds && z.name == name;
    });
    return all.length == 1;
}

function findCollectionInfo() {
    let all = db.getCollectionInfos();
    all = all.filter(function (z) {
        return z.name == t.getName();
    });
    assert.eq(all.length, 1);
    return all[0];
}

// ensure we fail with gibberish options
assert.commandFailed(t.runCommand("collMod", {NotARealOption: 1}));

// add a TTL index
t.createIndex({a: 1}, {"name": "index1", "expireAfterSeconds": 50});
assert(findTTL({a: 1}, 50), "TTL index not added");

// try to modify it with a bad key pattern
var res = db.runCommand({"collMod": coll, "index": {"keyPattern": "bad", "expireAfterSeconds": 100}});
debug(res);
assert.eq(0, res.ok, "mod shouldn't work with bad keypattern");

// Ensure collMod fails with a non-string indexName.
var res = db.runCommand({"collMod": coll, "index": {"indexName": 2, "expireAfterSeconds": 120}});
assert.commandFailed(res);

// try to modify it without expireAfterSeconds field
var res = db.runCommand({"collMod": coll, "index": {"keyPattern": {a: 1}}});
debug(res);
assert.eq(0, res.ok, "TTL mod shouldn't work without expireAfterSeconds");

// try to modify it with a non-numeric expireAfterSeconds field
var res = db.runCommand({"collMod": coll, "index": {"keyPattern": {a: 1}, "expireAfterSeconds": "100"}});
debug(res);
assert.eq(0, res.ok, "TTL mod shouldn't work with non-numeric expireAfterSeconds");

// this time modifying should finally  work
var res = db.runCommand({"collMod": coll, "index": {"keyPattern": {a: 1}, "expireAfterSeconds": 100}});
debug(res);
assert(findTTL({a: 1}, 100), "TTL index not modified");

// Modify ttl index by name.
var res = db.runCommand({"collMod": coll, "index": {"name": "index1", "expireAfterSeconds": 500}});
assert(findTTL({a: 1}, 500), "TTL index not modified");

// Must specify key pattern or name.
assert.commandFailed(db.runCommand({"collMod": coll, "index": {}}));

// Not allowed to specify key pattern and name.
assert.commandFailed(
    db.runCommand({
        "collMod": coll,
        "index": {"keyPattern": {a: 1}, "name": "index1", "expireAfterSeconds": 1000},
    }),
);

// try with new index, this time set expireAfterSeconds
t.dropIndex({a: 1});
t.createIndex({a: 1}, {"expireAfterSeconds": 50});
var res = db.runCommand({"collMod": coll, "index": {"keyPattern": {a: 1}, "expireAfterSeconds": 100}});
debug(res);
assert(findTTL({a: 1}, 100), "TTL index should be 100 now");

// Tests for collmod over multiple indexes with the same key pattern.
t.drop();
assert.commandWorked(db.createCollection(coll));
t = db.getCollection(coll);

// It's odd to create multiple TTL indexes... but you can.
assert.commandWorked(t.createIndex({a: 1}, {name: "TTL", expireAfterSeconds: 60}));
assert.commandWorked(t.createIndex({a: 1}, {name: "TTLfr", collation: {locale: "fr"}, expireAfterSeconds: 120}));

// Ensure that coll mod will not accept an ambiguous key pattern.
assert.commandFailed(db.runCommand({collMod: coll, index: {keyPattern: {a: 1}, expireAfterSeconds: 240}}));
assert(!findTTL({a: 1}, 240), "TTL index modified.");

// Ensure that a single TTL index is modified by name.
assert.commandWorked(db.runCommand({collMod: coll, index: {name: "TTL", expireAfterSeconds: 100}}));
assert(findTTLByName("TTL", 100), "TTL index not modified.");
assert(findTTLByName("TTLfr", 120), "TTL index modified.");

// Fails with an unknown index name.
assert.commandFailed(db.runCommand({collMod: coll, index: {name: "notaname", expireAfterSeconds: 100}}));

// Fails with an unknown key pattern.
assert.commandFailed(db.runCommand({collMod: coll, index: {keyPattern: {doesnotexist: 1}, expireAfterSeconds: 100}}));

// The timeseriesBucketsMayHaveMixedSchemaData option can only be used on time-series
// collections.
assert.commandFailedWithCode(
    db.runCommand({collMod: coll, timeseriesBucketsMayHaveMixedSchemaData: true}),
    ErrorCodes.InvalidOptions,
);
assert.commandFailedWithCode(
    db.runCommand({collMod: coll, timeseriesBucketsMayHaveMixedSchemaData: false}),
    ErrorCodes.InvalidOptions,
);
