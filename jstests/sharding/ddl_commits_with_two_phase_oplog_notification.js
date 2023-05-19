/**
 * Verifies that the successful commit of Sharding DDL operations implementing the "2-phase oplog"
 * notification generate the expected op entries.
 * @tags: [
 *   does_not_support_stepdowns,
 *   requires_fcv_70,
 * ]
 */
(function() {
load('jstests/libs/fail_point_util.js');
load('jstests/libs/parallel_shell_helpers.js');

const st = new ShardingTest({shards: 2, chunkSize: 1});

function verifyOpEntriesForDatabaseOnRS(dbName, isImported, dbPrimaryShard, replicaSet) {
    const primaryNodeOplog = replicaSet.getPrimary().getDB('local').oplog.rs;

    const latestInternalOpEntries =
        primaryNodeOplog.find({op: 'n', ns: dbName}).sort({ts: -1}).limit(2).toArray().reverse();
    assert.eq(2, latestInternalOpEntries.length);

    const prepareCommitEntry = latestInternalOpEntries[0];
    assert.eq(dbName, prepareCommitEntry.o.msg.createDatabasePrepare);
    assert.eq(dbName, prepareCommitEntry.o2.createDatabasePrepare);
    assert.eq(isImported, prepareCommitEntry.o2.isImported);
    assert.eq(dbPrimaryShard, prepareCommitEntry.o2.primaryShard);

    const commitSuccessfulEntry = latestInternalOpEntries[1];
    assert.eq(dbName, commitSuccessfulEntry.o.msg.createDatabase);
    assert.eq(dbName, commitSuccessfulEntry.o2.createDatabase);
    assert.eq(isImported, commitSuccessfulEntry.o2.isImported);
    assert.eq(undefined, commitSuccessfulEntry.o2.primaryShard);
}

function testCreateDatabase() {
    jsTest.log('test createDatabase');
    const dbName = 'createDatabaseTestDB';
    const primaryShard = st.rs0;
    const primaryShardId = st.shard0.shardName;

    // Execute enableSharding, injecting a stepdown of the config server between the write into the
    // sharding catalog and the remote notification of the "commitSuccessful" event. The command is
    // expected to eventually succeed.
    let failpointHandle =
        configureFailPoint(st.configRS.getPrimary(), 'hangBeforeNotifyingCreateDatabaseCommitted');

    const joinDatabaseCreation = startParallelShell(
        funWithArgs(function(dbName, primaryShardName) {
            assert.commandWorked(
                db.adminCommand({enableSharding: dbName, primaryShard: primaryShardName}));
        }, dbName, primaryShardId), st.s.port);

    failpointHandle.wait();
    assert.commandWorked(st.configRS.getPrimary().adminCommand(
        {replSetStepDown: 10 /* stepDownSecs */, force: true}));
    failpointHandle.off();

    // Allow enableSharding to finish.
    joinDatabaseCreation();

    // Despite the CSRS stepdown, the remote notification of each phase should have reached the
    // primary shard of the newly created database. As a consequence of this, a single op entry for
    // each phase should have been generated.
    verifyOpEntriesForDatabaseOnRS(dbName, false /*isImported*/, primaryShardId, primaryShard);
}

function testAddShard() {
    jsTest.log('Test addShard');

    // Create a new replica set and populate it with two DBs
    const newReplicaSet = new ReplSetTest({name: 'addedShard', nodes: 1});
    const newShardName = 'addedShard';
    const preExistingCollName = 'preExistingColl';
    newReplicaSet.startSet({shardsvr: ""});
    newReplicaSet.initiate();
    const dbsOnNewReplicaSet = ['addShardTestDB1', 'addShardTestDB2'];
    for (const dbName of dbsOnNewReplicaSet) {
        const db = newReplicaSet.getPrimary().getDB(dbName);
        assert.commandWorked(db[preExistingCollName].save({value: 1}));
    }

    // Execute addShard, injecting a stepdown of the config server between the write into the
    // sharding catalog and the remote notification of the "commitSuccessful" event. The command is
    // expected to eventually succeed.
    let failpointHandle =
        configureFailPoint(st.configRS.getPrimary(), 'hangBeforeNotifyingaddShardCommitted');

    const joinAddShard = startParallelShell(
        funWithArgs(function(newShardUrl, newShardName) {
            assert.commandWorked(db.adminCommand({addShard: newShardUrl, name: newShardName}));
        }, newReplicaSet.getURL(), newShardName), st.s.port);

    failpointHandle.wait();
    assert.commandWorked(st.configRS.getPrimary().adminCommand(
        {replSetStepDown: 10 /* stepDownSecs */, force: true}));
    failpointHandle.off();

    // Allow addShard to finish.
    joinAddShard();

    // Despite the CSRS stepdown, the remote notification of each phase should have reached each
    // pre-existing shard of the cluster. As a consequence of this, each shard should contain 2 op
    // entries for each database imported from the new RS as part of addShard.
    for (let existingShard of [st.rs0, st.rs1]) {
        for (let importedDB of dbsOnNewReplicaSet) {
            verifyOpEntriesForDatabaseOnRS(
                importedDB, true /*isImported*/, newShardName, existingShard);
        }
    }

    // Execute the test case teardown
    st.s.adminCommand({removeShard: newShardName});
    newReplicaSet.stopSet();
}

testCreateDatabase();

testAddShard();

st.stop();
}());
