(function() {
'use strict';

var s = new ShardingTest({});
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
let unshardedColl = db['unSharded'];

unshardedColl.insert({x: 1});
assert.commandFailedWithCode(
    db.adminCommand(
        {renameCollection: unshardedColl.getFullName(), to: 'otherDBDifferentPrimary.foo'}),
    [ErrorCodes.CommandFailed],
    "Source and destination collections must be on the same database.");

// Renaming unsharded collection to a different db with same primary shard.
assert.commandWorked(
    db.adminCommand({renameCollection: unshardedColl.getFullName(), to: 'otherDBSamePrimary.foo'}));
assert.eq(0, unshardedColl.countDocuments({}));
assert.eq(1, s.getDB('otherDBSamePrimary').foo.countDocuments({}));

jsTest.log("Testing that rename operations involving views are not allowed");
{
    assert.commandWorked(db.collForView.insert({_id: 1}));
    assert.commandWorked(db.createView('view', 'collForView', []));

    unshardedColl.insert({x: 1});
    let toAView = unshardedColl.renameCollection('view', true /* dropTarget */);

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

{
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
    s.shard1.getDB('test').badcollection.drop();

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

s.stop();
})();
