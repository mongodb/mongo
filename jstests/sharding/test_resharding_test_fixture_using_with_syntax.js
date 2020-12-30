/**
 * Test for the ReshardingTest fixture itself.
 *
 * Verifies that an uncaught exception in withReshardingInBackground() won't cause the mongo shell
 * to abort.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */
(function() {
"use strict";

if (_isWindows()) {
    jsTest.log("Skipping test on Windows because it makes assumptions about exit codes for" +
               " std::terminate()");
    return;
}

const awaitShell = startParallelShell(function() {
    load("jstests/sharding/libs/resharding_test_fixture.js");

    const reshardingTest = new ReshardingTest();
    reshardingTest.setup();

    const ns = "reshardingDb.coll";
    const donorShardNames = reshardingTest.donorShardNames;
    reshardingTest.createShardedCollection({
        ns,
        shardKeyPattern: {oldKey: 1},
        chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
    });

    const recipientShardNames = reshardingTest.recipientShardNames;
    reshardingTest.withReshardingInBackground(
        {
            newShardKeyPattern: {newKey: 1},
            newChunks: [
                {min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]},
            ],
        },
        () => {
            throw new Error("Intentionally throwing exception to simulate assertion failure");
        });
}, undefined, true);

const exitCode = awaitShell({checkExitSuccess: false});
assert.neq(exitCode, 0);
assert.neq(exitCode, MongoRunner.EXIT_ABORT);
})();
