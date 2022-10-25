// Checking UUID consistency involves talking to a shard node, which in this test is shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
'use strict';

load("jstests/replsets/rslib.js");

var s = new ShardingTest({shards: 2, mongos: 1, rs: {oplogSize: 10}});
var db = s.getDB("test");

assert.commandWorked(db.foo.insert({_id: 1}));
assert.commandWorked(db.foo.renameCollection('bar'));
assert.eq(db.bar.findOne(), {_id: 1}, '1.1');
assert.eq(db.bar.count(), 1, '1.2');
assert.eq(db.foo.count(), 0, '1.3');

assert.commandWorked(db.foo.insert({_id: 2}));
assert.commandWorked(db.foo.renameCollection('bar', true));
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

// Renaming to a sharded collection without dropTarget=true
assert.commandFailed(db.bar.renameCollection('shardedColl'));

// Renaming unsharded collection to a different db with different primary shard.
db.unSharded.insert({x: 1});
assert.commandFailedWithCode(
    db.adminCommand({renameCollection: 'test.unSharded', to: 'otherDBDifferentPrimary.foo'}),
    [ErrorCodes.CommandFailed],
    "Source and destination collections must be on the same database.");

// Renaming unsharded collection to a different db with same primary shard.
assert.commandWorked(
    db.adminCommand({renameCollection: 'test.unSharded', to: 'otherDBSamePrimary.foo'}));
assert.eq(0, db.unsharded.countDocuments({}));
assert.eq(1, s.getDB('otherDBSamePrimary').foo.countDocuments({}));

const testDB = s.rs0.getPrimary().getDB('test');
const fcvDoc = testDB.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
jsTest.log("Testing that rename operations involving views are not allowed");
{
    assert.commandWorked(db.collForView.insert({_id: 1}));
    assert.commandWorked(db.createView('view', 'collForView', []));

    let toAView = db.unsharded.renameCollection('view', true /* dropTarget */);

    assert.commandFailedWithCode(
        toAView,
        [
            ErrorCodes.NamespaceExists,
            ErrorCodes.CommandNotSupportedOnView,  // TODO SERVER-68084 remove this error code
            ErrorCodes.NamespaceNotFound           // TODO SERVER-68084 remove this error code
        ],
        "renameCollection should fail with NamespaceExists when the target is view");

    let fromAView = db.view.renameCollection('target');
    assert.commandFailedWithCode(
        fromAView,
        [
            ErrorCodes.CommandNotSupportedOnView,
            ErrorCodes.NamespaceNotFound  // TODO SERVER-68084 remove this error code
        ],
        "renameCollection should fail with CommandNotSupportedOnView when renaming a view");
}

// Rename a collection to itself fails, without loosing data
{
    const sameCollName = 'sameColl';
    const sameColl = db[sameCollName];
    assert.commandWorked(sameColl.insert({a: 1}));

    assert.commandFailedWithCode(sameColl.renameCollection(sameCollName, true /* dropTarget */),
                                 [ErrorCodes.IllegalOperation]);

    assert.eq(1, sameColl.countDocuments({}), "Rename a collection to itself must not loose data");
}

if (MongoRunner.compareBinVersions(fcvDoc.featureCompatibilityVersion.version, '6.1') >= 0) {
    // Create collection on non-primary shard (shard1 for test db) to simulate wrong creation via
    // direct connection: collection rename should fail since `badcollection` uuids are inconsistent
    // across shards
    jsTest.log("Testing uuid consistency across shards");
    assert.commandWorked(
        s.shard1.getDB('test').badcollection.insert({_id: 1}));               // direct connection
    assert.commandWorked(s.s0.getDB('test').badcollection.insert({_id: 1}));  // mongos connection
    assert.commandFailedWithCode(
        s.s0.getDB('test').badcollection.renameCollection('goodcollection'),
        [ErrorCodes.InvalidUUID],
        "collection rename should fail since test.badcollection uuids are inconsistent across shards");

    // Target collection existing on non-primary shard: rename with `dropTarget=false` must fail
    jsTest.log(
        "Testing rename behavior when target collection [wrongly] exists on non-primary shards");
    assert.commandWorked(
        s.shard1.getDB('test').superbadcollection.insert({_id: 1}));           // direct connection
    assert.commandWorked(s.s0.getDB('test').goodcollection.insert({_id: 1}));  // mongos connection
    assert.commandFailedWithCode(
        s.s0.getDB('test').goodcollection.renameCollection('superbadcollection', false),
        [ErrorCodes.NamespaceExists],
        "Collection rename with `dropTarget=false` must have failed because target collection exists on a non-primary shard");
    // Target collection existing on non-primary shard: rename with `dropTarget=true` must succeed
    assert.commandWorked(
        s.s0.getDB('test').goodcollection.renameCollection('superbadcollection', true));
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
