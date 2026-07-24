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

const kTransientBlockWriteContentionCodes = [
    ErrorCodes.LockBusy,
    ErrorCodes.LockTimeout,
    ErrorCodes.ConflictingOperationInProgress,
];

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
        if (res.ok) {
            return true;
        }
        // Only retry the expected transient contention codes; fail fast on anything else.
        assert.contains(res.code, kTransientBlockWriteContentionCodes, () => tojson(res));
        return false;
    }, "Failed to disable replica set write block");
}

/**
 * Disables the replica set write block on every shard.
 */
export function disableAllWriteBlocks(shardInfo, reason) {
    for (const shard of Object.keys(shardInfo.rsConns)) {
        if (shard === "config") {
            continue;
        }
        disableReplicaSetWriteBlock(shardInfo.rsConns[shard].getDB("admin"), reason);
    }
}
