/**
 * Tests that a last-lts secondary can apply collMod convert to ttl.
 */

(function() {
"use strict";
load('./jstests/multiVersion/libs/multi_rs.js');

const lastLTSVersion = "last-lts";
const latestVersion = "latest";

const nodes = {
    0: {binVersion: latestVersion},
    1: {binVersion: lastLTSVersion},
    2: {binVersion: lastLTSVersion}
};

const rst = new ReplSetTest({nodes: nodes});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const collName = 'mixed_replica_set_collmod_ttl';
const coll = testDB.getCollection(collName);
coll.drop();

function findTTL(coll, key, expireAfterSeconds) {
    let all = coll.getIndexes();
    all = all.filter(function(z) {
        return z.expireAfterSeconds == expireAfterSeconds && friendlyEqual(z.key, key);
    });
    return all.length == 1;
}

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(testDB.runCommand({
    collMod: collName,
    index: {keyPattern: {a: 1}, expireAfterSeconds: 100},
    writeConcern: {w: 3}
}));

for (let i = 0; i < rst.nodes.length; i++) {
    const coll = rst.nodes[i].getDB(testDB.getName()).getCollection(collName);
    assert(findTTL(coll, {a: 1}, 100), "TTL index should be 100 now");
}

rst.stopSet();
})();
