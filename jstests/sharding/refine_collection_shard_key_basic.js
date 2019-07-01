//
// Basic tests for refineCollectionShardKey.
//

(function() {
    'use strict';
    load('jstests/sharding/libs/sharded_transactions_helpers.js');

    const st = new ShardingTest({mongos: 2, shards: 1});
    const mongos = st.s0;
    const staleMongos = st.s1;
    const kDbName = 'db';
    const kCollName = 'foo';
    const kNsName = kDbName + '.' + kCollName;

    function enableShardingAndShardColl(keyDoc) {
        assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
        assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: keyDoc}));
    }

    function dropAndRecreateColl(keyDoc) {
        assert.commandWorked(mongos.getDB(kDbName).runCommand({drop: kCollName}));
        assert.writeOK(mongos.getCollection(kNsName).insert(keyDoc));
    }

    function dropAndReshardColl(keyDoc) {
        assert.commandWorked(mongos.getDB(kDbName).runCommand({drop: kCollName}));
        assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: keyDoc}));
    }

    function dropAndReshardCollUnique(keyDoc) {
        assert.commandWorked(mongos.getDB(kDbName).runCommand({drop: kCollName}));
        assert.commandWorked(
            mongos.adminCommand({shardCollection: kNsName, key: keyDoc, unique: true}));
    }

    // ********** SIMPLE TESTS **********

    // Should fail because arguments 'refineCollectionShardKey' and 'key' are invalid types.
    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: {_id: 1}, key: {_id: 1, aKey: 1}}),
        ErrorCodes.TypeMismatch);
    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: 'blah'}),
        ErrorCodes.TypeMismatch);

    // Should fail because refineCollectionShardKey may only be run against the admin database.
    assert.commandFailedWithCode(mongos.getDB(kDbName).runCommand(
                                     {refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
                                 ErrorCodes.Unauthorized);

    // Should fail because namespace 'db.foo' does not exist.
    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
        ErrorCodes.NamespaceNotFound);

    assert.writeOK(mongos.getCollection(kNsName).insert({aKey: 1}));

    // Should fail because namespace 'db.foo' is not sharded. NOTE: This NamespaceNotSharded error
    // is thrown in RefineCollectionShardKeyCommand by 'getShardedCollectionRoutingInfoWithRefresh'.
    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
        ErrorCodes.NamespaceNotSharded);

    enableShardingAndShardColl({_id: 1});

    // Should fail because shard key is invalid (i.e. bad values).
    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 5}}),
        ErrorCodes.BadValue);
    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: -1}}),
        ErrorCodes.BadValue);
    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 'hashed'}}),
        ErrorCodes.BadValue);
    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 'hahashed'}}),
        ErrorCodes.BadValue);

    // Should fail because shard key is not specified.
    assert.commandFailedWithCode(mongos.adminCommand({refineCollectionShardKey: kNsName}), 40414);
    assert.commandFailedWithCode(mongos.adminCommand({refineCollectionShardKey: kNsName, key: {}}),
                                 ErrorCodes.BadValue);

    // Should work because new shard key is already same as current shard key of namespace 'db.foo'.
    assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1}}));
    dropAndReshardColl({a: 1, b: 1});
    assert.commandWorked(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {a: 1, b: 1}}));
    dropAndReshardColl({aKey: 'hashed'});
    assert.commandWorked(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 'hashed'}}));

    assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

    // ********** NAMESPACE VALIDATION TESTS **********

    enableShardingAndShardColl({_id: 1});

    // Configure failpoint 'hangRefineCollectionShardKeyAfterRefresh' on staleMongos and run
    // refineCollectionShardKey against this mongos in a parallel thread.
    assert.commandWorked(staleMongos.adminCommand(
        {configureFailPoint: 'hangRefineCollectionShardKeyAfterRefresh', mode: 'alwaysOn'}));
    const awaitShellToTriggerNamespaceNotSharded = startParallelShell(() => {
        assert.commandFailedWithCode(
            db.adminCommand({refineCollectionShardKey: 'db.foo', key: {_id: 1, aKey: 1}}),
            ErrorCodes.NamespaceNotSharded);
    }, staleMongos.port);
    waitForFailpoint('Hit hangRefineCollectionShardKeyAfterRefresh', 1);

    // Drop and re-create namespace 'db.foo' without staleMongos refreshing its metadata.
    dropAndRecreateColl({aKey: 1});

    // Should fail because namespace 'db.foo' is not sharded. NOTE: This NamespaceNotSharded error
    // is thrown in ConfigsvrRefineCollectionShardKeyCommand.
    assert.commandWorked(staleMongos.adminCommand(
        {configureFailPoint: 'hangRefineCollectionShardKeyAfterRefresh', mode: 'off'}));
    awaitShellToTriggerNamespaceNotSharded();

    assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: {_id: 1}}));

    // Configure failpoint 'hangRefineCollectionShardKeyAfterRefresh' on staleMongos and run
    // refineCollectionShardKey against this mongos in a parallel thread.
    assert.commandWorked(staleMongos.adminCommand(
        {configureFailPoint: 'hangRefineCollectionShardKeyAfterRefresh', mode: 'alwaysOn'}));
    const awaitShellToTriggerStaleEpoch = startParallelShell(() => {
        assert.commandFailedWithCode(
            db.adminCommand({refineCollectionShardKey: 'db.foo', key: {_id: 1, aKey: 1}}),
            ErrorCodes.StaleEpoch);
    }, staleMongos.port);
    waitForFailpoint('Hit hangRefineCollectionShardKeyAfterRefresh', 2);

    // Drop and re-shard namespace 'db.foo' without staleMongos refreshing its metadata.
    dropAndReshardColl({_id: 1});

    // Should fail because staleMongos has a stale epoch.
    assert.commandWorked(staleMongos.adminCommand(
        {configureFailPoint: 'hangRefineCollectionShardKeyAfterRefresh', mode: 'off'}));
    awaitShellToTriggerStaleEpoch();

    assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

    // ********** SHARD KEY VALIDATION TESTS **********

    enableShardingAndShardColl({_id: 1});

    // Should fail because new shard key {aKey: 1} does not extend current shard key {_id: 1} of
    // namespace 'db.foo'.
    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 1}}),
        ErrorCodes.InvalidOptions);

    // Should fail because no index exists for new shard key {_id: 1, aKey: 1}.
    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
        ErrorCodes.InvalidOptions);

    // Should fail because only a sparse index exists for new shard key {_id: 1, aKey: 1}.
    dropAndReshardColl({_id: 1});
    assert.commandWorked(
        mongos.getCollection(kNsName).createIndex({_id: 1, aKey: 1}, {sparse: true}));

    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
        ErrorCodes.InvalidOptions);

    // Should fail because only a partial index exists for new shard key {_id: 1, aKey: 1}.
    dropAndReshardColl({_id: 1});
    assert.commandWorked(mongos.getCollection(kNsName).createIndex(
        {_id: 1, aKey: 1}, {partialFilterExpression: {aKey: {$gt: 0}}}));

    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
        ErrorCodes.OperationFailed);

    // Should fail because only a multikey index exists for new shard key {_id: 1, aKey: 1}.
    dropAndReshardColl({_id: 1});
    assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: 1, aKey: 1}));
    assert.writeOK(mongos.getCollection(kNsName).insert({aKey: [1, 2, 3, 4, 5]}));

    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
        ErrorCodes.OperationFailed);

    // Should fail because current shard key {a: 1} is unique, new shard key is {a: 1, b: 1}, and an
    // index only exists on {a: 1, b: 1, c: 1}.
    dropAndReshardCollUnique({a: 1});
    assert.commandWorked(mongos.getCollection(kNsName).createIndex({a: 1, b: 1, c: 1}));

    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {a: 1, b: 1}}),
        ErrorCodes.InvalidOptions);

    // Should work because current shard key {_id: 1} is not unique, new shard key is {_id: 1, aKey:
    // 1}, and an index exists on {_id: 1, aKey: 1, bKey: 1}.
    dropAndReshardColl({_id: 1});
    assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: 1, aKey: 1, bKey: 1}));

    assert.commandWorked(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}));

    // Should fail because only an index with missing or incomplete shard key entries exists for new
    // shard key {_id: 1, aKey: 1}.
    dropAndReshardColl({_id: 1});
    assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: 1, aKey: 1}));
    assert.writeOK(mongos.getCollection(kNsName).insert({_id: 12345}));

    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}),
        ErrorCodes.OperationFailed);

    // Should fail because new shard key {aKey: 1} is not a prefix of current shard key {_id: 1,
    // aKey: 1}.
    dropAndReshardColl({_id: 1, aKey: 1});
    assert.commandWorked(mongos.getCollection(kNsName).createIndex({aKey: 1}));

    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 1}}),
        ErrorCodes.InvalidOptions);

    // Should fail because new shard key {aKey: 1, _id: 1} is not a prefix of current shard key
    // {_id: 1, aKey: 1}.
    dropAndReshardColl({_id: 1, aKey: 1});
    assert.commandWorked(mongos.getCollection(kNsName).createIndex({aKey: 1, _id: 1}));

    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 1, _id: 1}}),
        ErrorCodes.InvalidOptions);

    // Should fail because new shard key {aKey: 1, _id: 1, bKey: 1} is not a prefix of current shard
    // key {_id: 1, aKey: 1}.
    dropAndReshardColl({_id: 1, aKey: 1});
    assert.commandWorked(mongos.getCollection(kNsName).createIndex({aKey: 1, _id: 1, bKey: 1}));

    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 1, _id: 1, bKey: 1}}),
        ErrorCodes.InvalidOptions);

    // Should fail because new shard key {aKey: 1, bKey: 1} is not a prefix of current shard key
    // {_id: 1}.
    dropAndReshardColl({_id: 1});
    assert.commandWorked(mongos.getCollection(kNsName).createIndex({aKey: 1, bKey: 1}));

    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 1, bKey: 1}}),
        ErrorCodes.InvalidOptions);

    // Should work because a 'useful' index exists for new shard key {_id: 1, aKey: 1}.
    dropAndReshardColl({_id: 1});
    assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: 1, aKey: 1}));

    assert.commandWorked(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, aKey: 1}}));

    assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

    st.stop();
})();
