// @tags: [
//   uses_parallel_shell,
// ]

// SERVER-48068: check that runningChildPids doesn't unregister a pid multiple
// times from the registry
// Verify that an invariant failure doesn't occur in the program registry
try {
    var cleanup = startParallelShell("MongoRunner.runningChildPids();", undefined, true);
    var cleanup2 = startParallelShell("MongoRunner.runningChildPids();", undefined, true);
    sleep(5000);

    try {
        MongoRunner.runningChildPids();
        throw new Error('Simulating assert.soon() failure');
    } finally {
        cleanup();
        cleanup2();
    }

} catch (e) {
    assert.eq(e instanceof Error, true);
    assert.eq(e.message, 'Simulating assert.soon() failure');
}

print("shell_parallel_wait_for_pid.js SUCCESS");
