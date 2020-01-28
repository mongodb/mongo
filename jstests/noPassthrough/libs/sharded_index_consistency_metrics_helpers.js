// Helper functions for verifying the serverStatus output that tracks inconsistent indexes.

/**
 * Returns the count of sharded collections with inconsistent indexes from the serverStatus output
 * of the given connection. Throws if serverStatus does not include the count.
 */
function getServerStatusNumCollsWithInconsistentIndexes(conn) {
    const res = assert.commandWorked(conn.adminCommand({serverStatus: 1}));
    assert.hasFields(res, ["shardedIndexConsistency"]);
    assert.hasFields(res.shardedIndexConsistency, ["numShardedCollectionsWithInconsistentIndexes"]);
    return res.shardedIndexConsistency.numShardedCollectionsWithInconsistentIndexes;
}

/*
 * Asserts that eventually the number of sharded collections with inconsistent indexes in the
 * serverStatus output is equal to the expected count.
 */
function checkServerStatusNumCollsWithInconsistentIndexes(conn, expectedCount, timeout) {
    assert.soon(
        () => {
            return expectedCount == getServerStatusNumCollsWithInconsistentIndexes(conn);
        },
        `expected count of sharded collections with inconsistent indexes to eventually equal ${
            expectedCount}`,
        timeout /* timeout */,
        1000 /* interval */);
}
