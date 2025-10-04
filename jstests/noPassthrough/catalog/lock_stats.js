// SERVER-21305: reported lock times are way too high.
//
// This test uses the fsync command to induce locking.
// @tags: [requires_fsync]
function testBlockTime(blockTimeMillis) {
    // Lock the database, and in parallel start an operation that needs the lock, so it blocks.
    assert.commandWorked(db.fsyncLock());
    let startStats = db.serverStatus().locks.MultiDocumentTransactionsBarrier;
    let startTime = new Date();
    let minBlockedMillis = blockTimeMillis;

    let awaitSleepCmd = startParallelShell(() => {
        assert.commandWorked(db.adminCommand({sleep: 1, millis: 100, lock: "w", $comment: "Lock sleep"}));
    }, conn.port);

    // Wait until we see somebody waiting to acquire the lock, defend against unset stats.
    assert.soon(function () {
        let stats = db.serverStatus().locks.MultiDocumentTransactionsBarrier;
        if (!stats.acquireWaitCount || !stats.acquireWaitCount.W) return false;
        if (!stats.timeAcquiringMicros || !stats.timeAcquiringMicros.W) return false;
        if (!startStats.acquireWaitCount || !startStats.acquireWaitCount.W) return true;
        return stats.acquireWaitCount.W > startStats.acquireWaitCount.W;
    });

    // Sleep for minBlockedMillis, so the acquirer would have to wait at least that long.
    sleep(minBlockedMillis);
    db.fsyncUnlock();

    awaitSleepCmd();

    // The fsync command from the shell cannot have possibly been blocked longer than this.
    let maxBlockedMillis = new Date() - startTime;
    let endStats = db.serverStatus().locks.MultiDocumentTransactionsBarrier;

    //  The server was just started, so initial stats may be missing.
    if (!startStats.acquireWaitCount || !startStats.acquireWaitCount.W) {
        startStats.acquireWaitCount = {W: 0};
    }
    if (!startStats.timeAcquiringMicros || !startStats.timeAcquiringMicros.W) {
        startStats.timeAcquiringMicros = {W: 0};
    }

    let acquireWaitCount = endStats.acquireWaitCount.W - startStats.acquireWaitCount.W;
    let blockedMillis = Math.floor((endStats.timeAcquiringMicros.W - startStats.timeAcquiringMicros.W) / 1000);

    // Require that no other commands run (and maybe acquire locks) in parallel.
    assert.eq(acquireWaitCount, 1, "other commands ran in parallel, can't check timing");
    assert.gte(blockedMillis, minBlockedMillis, "reported time acquiring lock is too low");
    assert.lte(blockedMillis, maxBlockedMillis, "reported time acquiring lock is too high");
    return {
        blockedMillis: blockedMillis,
        minBlockedMillis: minBlockedMillis,
        maxBlockedMillis: maxBlockedMillis,
    };
}

var conn = MongoRunner.runMongod();
var db = conn.getDB("test");
printjson([1, 10, 100, 500, 1000, 1500].map(testBlockTime));
MongoRunner.stopMongod(conn);
