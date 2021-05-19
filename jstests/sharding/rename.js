// Checking UUID consistency involves talking to a shard node, which in this test is shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
'use strict';

load("jstests/replsets/rslib.js");

var s = new ShardingTest({shards: 2, mongos: 1, rs: {oplogSize: 10}});
var db = s.getDB("test");

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

assert.commandWorked(s.s0.adminCommand({enablesharding: 'test'}));
s.ensurePrimaryShard('test', s.shard0.shardName);
assert.commandWorked(
    s.s0.adminCommand({enablesharding: 'otherDBSamePrimary', primaryShard: s.shard0.shardName}));

assert.commandWorked(s.s0.adminCommand(
    {enablesharding: 'otherDBDifferentPrimary', primaryShard: s.shard1.shardName}));

jsTest.log('Testing renaming sharded collections');
assert.commandWorked(
    s.s0.adminCommand({shardCollection: 'test.shardedColl', key: {_id: 'hashed'}}));

const DDLFeatureFlagParam = assert.commandWorked(
    s.configRS.getPrimary().adminCommand({getParameter: 1, featureFlagShardingFullDDLSupport: 1}));
const isDDLFeatureFlagEnabled = DDLFeatureFlagParam.featureFlagShardingFullDDLSupport.value;
// Ensure renaming to or from a sharded collection fails in the legacy path.
if (!isDDLFeatureFlagEnabled) {
    // Renaming from a sharded collection
    assert.commandFailed(db.shardedColl.renameCollection('somethingElse'));

    // Renaming to a sharded collection with dropTarget=true
    const dropTarget = true;
    assert.commandFailed(db.bar.renameCollection('shardedColl', dropTarget));
}

// Renaming to a sharded collection without dropTarget=true
assert.commandFailed(db.bar.renameCollection('shardedColl'));

// Renaming unsharded collection to a different db with different primary shard.
db.unSharded.insert({x: 1});
assert.commandFailedWithCode(
    db.adminCommand({renameCollection: 'test.unSharded', to: 'otherDBDifferentPrimary.foo'}),
    // TODO SERVER-54879 just check for ErrorCodes.CommandFailed
    [ErrorCodes.CommandFailed, 13137],
    "Source and destination collections must be on the same database.");

// Renaming unsharded collection to a different db with same primary shard.
assert.commandWorked(
    db.adminCommand({renameCollection: 'test.unSharded', to: 'otherDBSamePrimary.foo'}));
assert.eq(0, db.unsharded.countDocuments({}));
assert.eq(1, s.getDB('otherDBSamePrimary').foo.countDocuments({}));

jsTest.log("Testing that rename operations involving views are not allowed");
{
    assert.commandWorked(db.collForView.insert({_id: 1}));
    assert.commandWorked(db.createView('view', 'collForView', []));

    let toAView = db.unsharded.renameCollection('view', true /* dropTarget */);
    assert.commandFailed(toAView);

    let fromAView = db.view.renameCollection('target');
    assert.commandFailed(fromAView);
}

// Rename a collection to itself fails, without loosing data
{
    const sameCollName = 'sameColl';
    const sameColl = db[sameCollName];
    assert.commandWorked(sameColl.insert({a: 1}));

    assert.commandFailedWithCode(sameColl.renameCollection(sameCollName, true /* dropTarget */),
                                 [ErrorCodes.IllegalOperation, ErrorCodes.NamespaceNotFound]);

    assert.eq(1, sameColl.countDocuments({}), "Rename a collection to itself must not loose data");
}

// Ensure write concern works by shutting down 1 node in a replica set shard
jsTest.log("Testing write concern (2)");

var replTest = s.rs0;

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

s.stop();
})();
