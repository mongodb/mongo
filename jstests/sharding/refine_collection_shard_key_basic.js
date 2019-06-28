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

    assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

    // refineCollectionShardKey should fail because namespace 'db.foo' is not sharded.
    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 1}}),
        ErrorCodes.NamespaceNotSharded);

    // refineCollectionShardKey should work because namespace 'db.foo' is sharded.
    assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: {_id: 1}}));
    assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 1}}));

    // Configure failpoint 'hangRefineCollectionShardKeyAfterRefresh' on staleMongos and run
    // refineCollectionShardKey against this mongos in a parallel thread.
    assert.commandWorked(staleMongos.adminCommand(
        {configureFailPoint: 'hangRefineCollectionShardKeyAfterRefresh', mode: 'alwaysOn'}));
    const awaitShell = startParallelShell(() => {
        assert.commandFailedWithCode(
            db.adminCommand({refineCollectionShardKey: 'db.foo', key: {aKey: 1}}),
            ErrorCodes.StaleEpoch);
    }, staleMongos.port);
    waitForFailpoint('Hit hangRefineCollectionShardKeyAfterRefresh', 1);

    // Drop and re-shard namespace 'db.foo' without staleMongos refreshing its metadata.
    assert.commandWorked(mongos.getDB(kDbName).runCommand({drop: kCollName}));
    assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: {_id: 1}}));

    // refineCollectionShardKey should fail because staleMongos has a stale epoch.
    assert.commandWorked(staleMongos.adminCommand(
        {configureFailPoint: 'hangRefineCollectionShardKeyAfterRefresh', mode: 'off'}));
    awaitShell();

    // refineCollectionShardKey should work because mongos has the current epoch.
    assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: {aKey: 1}}));

    st.stop();
})();
