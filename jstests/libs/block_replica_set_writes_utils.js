/**
 * Shared utilities for tests of the blockReplicaSetWrites command.
 */

export function enableReplicaSetWriteBlock(adminDB, allowDeletions, reason) {
    assert.commandWorked(
        adminDB.runCommand({
            blockReplicaSetWrites: 1,
            enabled: true,
            allowDeletions: allowDeletions,
            reason: reason,
        }),
    );
}

export function disableReplicaSetWriteBlock(adminDB, reason) {
    // Wait for replica set write block to be disabled. This is necessarry because the command can fail
    // transiently with a lock contantion with previous operations. For example, when the range deleter
    // background thread becomes active, it hits ReplicaSetWritesBlocked error and retries in a tight loop
    // without releasing its collection lock, so when disableReplicaSetWriteBlock is called immediately after,
    // it can transiently fail due to lock contention.
    assert.soon(() => {
        const res = adminDB.runCommand({
            blockReplicaSetWrites: 1,
            enabled: false,
            reason: reason,
        });
        return res.ok;
    }, "Failed to disable replica set write block");
}
