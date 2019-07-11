/**
 * Verify the behavior of dropping TTL index.
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

// Drop the TTL index.
assert.commandWorked(coll.dropIndex({x: 1}));

// Re-insert 50 docs with timestamp 'now - 24h'.
for (let i = 0; i < 50; i++) {
    assert.commandWorked(db.runCommand({insert: 'ttl_coll', documents: [{x: past}]}));
}

var ttlPasses = db.serverStatus().metrics.ttl.passes;
assert.soon(function() {
    return db.serverStatus().metrics.ttl.passes > ttlPasses;
});

assert.eq(coll.find().itcount(), 50);

MongoRunner.stopMongod(conn);
})();
