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
t.ensureIndex({a: 1}, {"expireAfterSeconds": 50});
assert(findTTL({a: 1}, 50), "TTL index not added");

// try to modify it with a bad key pattern
var res =
    db.runCommand({"collMod": coll, "index": {"keyPattern": "bad", "expireAfterSeconds": 100}});
debug(res);
assert.eq(0, res.ok, "mod shouldn't work with bad keypattern");

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
