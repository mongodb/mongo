// Checking UUID consistency involves talking to a shard node, which in this test is shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
'use strict';

load("jstests/replsets/rslib.js");

var s = new ShardingTest({shards: 2, mongos: 1, rs: {oplogSize: 10}});

var db = s.getDB("test");
var replTest = s.rs0;

assert.commandWorked(db.foo.insert({_id: 1}));
db.foo.renameCollection('bar');
assert.isnull(db.getLastError(), '1.0');
assert.eq(db.bar.findOne(), {_id: 1}, '1.1');
assert.eq(db.bar.count(), 1, '1.2');
assert.eq(db.foo.count(), 0, '1.3');

assert.commandWorked(db.foo.insert({_id: 2}));
db.foo.renameCollection('bar', true);
assert.isnull(db.getLastError(), '2.0');
assert.eq(db.bar.findOne(), {_id: 2}, '2.1');
assert.eq(db.bar.count(), 1, '2.2');
assert.eq(db.foo.count(), 0, '2.3');

assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
s.ensurePrimaryShard('test', s.shard0.shardName);

assert.commandWorked(s.s0.adminCommand({enablesharding: "samePrimary"}));
s.ensurePrimaryShard('samePrimary', s.shard0.shardName);

assert.commandWorked(s.s0.adminCommand({enablesharding: "otherPrimary"}));
s.ensurePrimaryShard('otherPrimary', s.shard1.shardName);

const DDLFeatureFlagParam = assert.commandWorked(
    s.configRS.getPrimary().adminCommand({getParameter: 1, featureFlagShardingFullDDLSupport: 1}));
const isDDLFeatureFlagEnabled = DDLFeatureFlagParam.featureFlagShardingFullDDLSupport.value;
if (!isDDLFeatureFlagEnabled) {
    // Ensure renaming to or from a sharded collection fails.
    jsTest.log('Testing renaming sharded collections');
    assert.commandWorked(
        s.s0.adminCommand({shardCollection: 'test.shardedColl', key: {_id: 'hashed'}}));

    // Renaming from a sharded collection
    assert.commandFailed(db.shardedColl.renameCollection('somethingElse'));

    // Renaming to a sharded collection
    assert.commandFailed(db.bar.renameCollection('shardedColl'));

    // Renaming to a sharded collection with dropTarget=true
    const dropTarget = true;
    assert.commandFailed(db.bar.renameCollection('shardedColl', dropTarget));
}

// Renaming unsharded collection to a different db with different primary shard.
db.unSharded.insert({x: 1});
assert.commandFailedWithCode(
    db.adminCommand({renameCollection: 'test.unSharded', to: 'otherPrimary.foo'}), 13137);

// Renaming unsharded collection to a different db with same primary shard.
assert.commandWorked(db.adminCommand({renameCollection: 'test.unSharded', to: 'samePrimary.foo'}));

jsTest.log("Testing write concern (1)");

assert.commandWorked(db.foo.insert({_id: 3}));
db.foo.renameCollection('bar', true);

var ans = db.runCommand({getLastError: 1, w: 3});
printjson(ans);
assert.isnull(ans.err, '3.0');

assert.eq(db.bar.findOne(), {_id: 3}, '3.1');
assert.eq(db.bar.count(), 1, '3.2');
assert.eq(db.foo.count(), 0, '3.3');

// Ensure write concern works by shutting down 1 node in a replica set shard
jsTest.log("Testing write concern (2)");

// Kill any node. Don't care if it's a primary or secondary.
replTest.stop(0);

// Call getPrimary() to populate replTest._secondaries.
replTest.getPrimary();
let liveSecondaries = replTest.getSecondaries().filter(function(node) {
    return node.host !== replTest.nodes[0].host;
});
replTest.awaitSecondaryNodes(null, liveSecondaries);
awaitRSClientHosts(s.s, replTest.getPrimary(), {ok: true, ismaster: true}, replTest.name);

assert.commandWorked(db.foo.insert({_id: 4}));
assert.commandWorked(db.foo.renameCollection('bar', true));

ans = db.runCommand({getLastError: 1, w: 3, wtimeout: 5000});
assert.eq(ans.err, "timeout", 'gle: ' + tojson(ans));

s.stop();
})();
