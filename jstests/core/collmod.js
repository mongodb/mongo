// Basic js tests for the collMod command.
// Test setting the usePowerOf2Sizes flag, and modifying TTL indexes.

function debug(x) {
    // printjson( x );
}

var coll = "collModTest";
var t = db.getCollection(coll);
t.drop();

var isMongos = ("isdbgrid" == db.runCommand("ismaster").msg);

db.createCollection(coll);

function findTTL(key, expireAfterSeconds) {
    var all = t.getIndexes();
    all = all.filter(function(z) {
        return z.expireAfterSeconds == expireAfterSeconds && friendlyEqual(z.key, key);
    });
    return all.length == 1;
}

function findTTLByName(name, expireAfterSeconds) {
    var all = t.getIndexes();
    all = all.filter(function(z) {
        return z.expireAfterSeconds == expireAfterSeconds && z.name == name;
    });
    return all.length == 1;
}

function findCollectionInfo() {
    var all = db.getCollectionInfos();
    all = all.filter(function(z) {
        return z.name == t.getName();
    });
    assert.eq(all.length, 1);
    return all[0];
}

// ensure we fail with gibberish options
assert.commandFailed(t.runCommand('collmod', {NotARealOption: 1}));

// add a TTL index
t.ensureIndex({a: 1}, {"name": "index1", "expireAfterSeconds": 50});
assert(findTTL({a: 1}, 50), "TTL index not added");

// try to modify it with a bad key pattern
var res =
    db.runCommand({"collMod": coll, "index": {"keyPattern": "bad", "expireAfterSeconds": 100}});
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
var res =
    db.runCommand({"collMod": coll, "index": {"keyPattern": {a: 1}, "expireAfterSeconds": "100"}});
debug(res);
assert.eq(0, res.ok, "TTL mod shouldn't work with non-numeric expireAfterSeconds");

// this time modifying should finally  work
var res =
    db.runCommand({"collMod": coll, "index": {"keyPattern": {a: 1}, "expireAfterSeconds": 100}});
debug(res);
assert(findTTL({a: 1}, 100), "TTL index not modified");

// Modify ttl index by name.
var res = db.runCommand({"collMod": coll, "index": {"name": "index1", "expireAfterSeconds": 500}});
assert(findTTL({a: 1}, 500), "TTL index not modified");

// Must specify key pattern or name.
assert.commandFailed(db.runCommand({"collMod": coll, "index": {}}));

// Not allowed to specify key pattern and name.
assert.commandFailed(db.runCommand({
    "collMod": coll,
    "index": {"keyPattern": {a: 1}, "name": "index1", "expireAfterSeconds": 1000}
}));

// try to modify a faulty TTL index with a non-numeric expireAfterSeconds field
t.dropIndex({a: 1});
t.ensureIndex({a: 1}, {"expireAfterSeconds": "50"});
var res =
    db.runCommand({"collMod": coll, "index": {"keyPattern": {a: 1}, "expireAfterSeconds": 100}});
debug(res);
assert.eq(0, res.ok, "shouldn't be able to modify faulty index spec");

// try with new index, this time set both expireAfterSeconds and the usePowerOf2Sizes flag
t.dropIndex({a: 1});
t.ensureIndex({a: 1}, {"expireAfterSeconds": 50});
var res = db.runCommand({
    "collMod": coll,
    "usePowerOf2Sizes": true,
    "index": {"keyPattern": {a: 1}, "expireAfterSeconds": 100}
});
debug(res);
assert(findTTL({a: 1}, 100), "TTL index should be 100 now");

// Clear usePowerOf2Sizes and enable noPadding. Make sure collection options.flags is updated.
var res = db.runCommand({"collMod": coll, "usePowerOf2Sizes": false, "noPadding": true});
debug(res);
assert.commandWorked(res);
var info = findCollectionInfo();
assert.eq(info.options.flags, 2, tojson(info));  // 2 is CollectionOptions::Flag_NoPadding

// Clear noPadding and check results object and options.flags.
var res = db.runCommand({"collMod": coll, "noPadding": false});
debug(res);
assert.commandWorked(res);
if (!isMongos) {
    // don't check this for sharding passthrough since mongos has a different output format.
    assert.eq(res.noPadding_old, true, tojson(res));
    assert.eq(res.noPadding_new, false, tojson(res));
}
var info = findCollectionInfo();
assert.eq(info.options.flags, 0, tojson(info));

// Tests for collmod over multiple indexes with the same key pattern.
t.drop();
assert.commandWorked(db.createCollection(coll));
t = db.getCollection(coll);

// It's odd to create multiple TTL indexes... but you can.
assert.commandWorked(t.createIndex({a: 1}, {name: "TTL", expireAfterSeconds: 60}));
assert.commandWorked(
    t.createIndex({a: 1}, {name: "TTLfr", collation: {locale: "fr"}, expireAfterSeconds: 120}));

// Ensure that coll mod will not accept an ambiguous key pattern.
assert.commandFailed(
    db.runCommand({collMod: coll, index: {keyPattern: {a: 1}, expireAfterSeconds: 240}}));
assert(!findTTL({a: 1}, 240), "TTL index modified.");

// Ensure that a single TTL index is modified by name.
assert.commandWorked(db.runCommand({collMod: coll, index: {name: "TTL", expireAfterSeconds: 100}}));
assert(findTTLByName("TTL", 100), "TTL index not modified.");
assert(findTTLByName("TTLfr", 120), "TTL index modified.");

// Fails with an unknown index name.
assert.commandFailed(
    db.runCommand({collMod: coll, index: {name: "notaname", expireAfterSeconds: 100}}));

// Fails with an unknown key pattern.
assert.commandFailed(db.runCommand(
    {collmod: coll, index: {keyPattern: {doesnotexist: 1}, expireAfterSeconds: 100}}));
