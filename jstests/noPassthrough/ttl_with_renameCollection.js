/**
 * Since ttl mechanism uses UUIDs to keep track of collections that have TTL indexes, nothing needs
 * to be done during a collection rename. This test is to verify the TTL behavior after a collection
 * rename.
 */
(function() {
'use strict';

let conn = MongoRunner.runMongod({setParameter: 'ttlMonitorSleepSecs=1'});
let db = conn.getDB('test');
let coll = db.ttl_coll;
coll.drop();
let now = (new Date()).getTime();

// Insert 50 docs with timestamp 'now - 24h'.
let past = new Date(now - (3600 * 1000 * 24));
for (let i = 0; i < 50; i++) {
    assert.commandWorked(db.runCommand({insert: 'ttl_coll', documents: [{x: past}]}));
}

assert.eq(coll.find().itcount(), 50);

// Create TTL index: expire docs older than 20000 seconds (~5.5h).
coll.createIndex({x: 1}, {expireAfterSeconds: 20000});

assert.soon(function() {
    return coll.find().itcount() == 0;
}, 'TTL index on x didn\'t delete');

// Rename the collection
assert.commandWorked(db.adminCommand(
    {renameCollection: 'test.ttl_coll', to: 'test.ttl_coll_renamed', dropTarget: true}));

// Re-insert 50 docs with timestamp 'now - 24h'.
for (let i = 0; i < 50; i++) {
    assert.commandWorked(db.runCommand({insert: 'ttl_coll_renamed', documents: [{x: past}]}));
}

// Assert that the TTL mechanism still works on this collection.
assert.soon(function() {
    return coll.find().itcount() == 0;
}, 'TTL index on x didn\'t delete');

MongoRunner.stopMongod(conn);
})();
