/**
 * Selftest for suites that enable the ContinuousAddRemoveShard hook. Verifies that the hook
 * completes a full add/remove cycle by observing that the listShards count first diverges
 * from its initial value, then returns to it. If the hook is silently stuck at any point
 * (a logic bug that keeps it looping without hitting its internal retry timeout), one of
 * the two assertions exposes the hang.
 *
 * We check for *any* difference (`!==`) rather than a decrease (`<`) so the test remains
 * correct regardless of whether the cycle starts with a remove or an add.
 *
 * The hook sleeps for `TRANSITION_INTERVALS` (10) seconds between the remove and the matching
 * add, so the transient phase is long enough for `assert.soon` to observe.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */

function shardCount(db) {
    return assert.commandWorked(db.adminCommand({listShards: 1})).shards.length;
}

const initialCount = shardCount(db);

jsTest.log(
    `ContinuousAddRemoveShard selftest: initial shard count is ${initialCount}. ` +
        `Waiting for a full add/remove cycle.`,
);

assert.soon(
    () => shardCount(db) !== initialCount,
    `ContinuousAddRemoveShard hook appears stuck: shard count never changed`,
);

jsTest.log("ContinuousAddRemoveShard selftest: shard count changed. Waiting for it to return to initial.");

assert.soon(
    () => shardCount(db) === initialCount,
    `ContinuousAddRemoveShard hook appears stuck: shard count changed but never returned to initial`,
);

jsTest.log("ContinuousAddRemoveShard selftest: full add/remove cycle completed successfully.");
