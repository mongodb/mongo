// This test validates that if a move chunk operation is active on a shard, that shard will not
// accept another move chunk operation
(function() {
    'use strict';

    load('jstests/libs/chunk_manipulation_util.js');

    var st = new ShardingTest({shards: 2});

    assert.commandWorked(st.s0.adminCommand({enableSharding: 'TestDB'}));
    st.ensurePrimaryShard('TestDB', st.shard0.shardName);
    assert.commandWorked(st.s0.adminCommand({shardCollection: 'TestDB.coll', key: {_id: 1}}));

    assert.writeOK(st.getDB('TestDB').coll.insert({_id: 0, value: 'Test value'}));

    // Enable the failpoint which will cause the move chunk command to hang right after taking the
    // distributed lock (there is no specific reason to pick that failpoint, any will suffice).
    pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.gotDistLock);

    // Schedule a migration, which will block
    var originalMoveChunkShell = startParallelShell(`(function() {
        'use strict';
        assert.commandWorked(
            db.adminCommand({moveChunk: 'TestDB.coll', find: {_id: 0}, to: 'shard0001'}));
    })()`,
                                                    st.s0.port);

    // Wait until the moveChunk command shows up in the operations list
    waitForMoveChunkStep(st.shard0, moveChunkStepNames.gotDistLock);

    // Now, while the moveChunk request is blocked, schedule a concurrent one, which should fail
    // with ConflictingOperationInProgress.
    assert.commandFailedWithCode(
        st.s0.adminCommand({moveChunk: 'TestDB.coll', find: {_id: 0}, to: 'shard0001'}),
        ErrorCodes.ConflictingOperationInProgress);

    // Disable the failpoint so the test can complete
    unpauseMoveChunkAtStep(st.shard0, moveChunkStepNames.gotDistLock);
    originalMoveChunkShell();

    st.stop();
})();
