// SERVER-21305: reported lock times are way too high.
//
// This test uses the fsync command to induce locking.
// @tags: [requires_fsync]
(function() {
    'use strict';

    function testBlockTime(blockTimeMillis) {
        // Lock the database, and in parallel start an operation that needs the lock, so it blocks.
        assert.commandWorked(db.fsyncLock());
        var startStats = db.serverStatus().locks.Global;
        var startTime = new Date();
        var minBlockedMillis = blockTimeMillis;
        var s = startParallelShell('assert.commandWorked(db.adminCommand({fsync:1}));', conn.port);

        // Wait until we see somebody waiting to acquire the lock, defend against unset stats.
        assert.soon((function() {
            var stats = db.serverStatus().locks.Global;
            if (!stats.acquireWaitCount || !stats.acquireWaitCount.W)
                return false;
            if (!startStats.acquireWaitCount || !startStats.acquireWaitCount.W)
                return true;
            return stats.acquireWaitCount.W > startStats.acquireWaitCount.W;
        }));

        // Sleep for minBlockedMillis, so the acquirer would have to wait at least that long.
        sleep(minBlockedMillis);
        db.fsyncUnlock();

        // Wait for the parallel shell to finish, so its stats will have been recorded.
        s();

        // The fsync command from the shell cannot have possibly been blocked longer than this.
        var maxBlockedMillis = new Date() - startTime;
        var endStats = db.serverStatus().locks.Global;

        //  The server was just started, so initial stats may be missing.
        if (!startStats.acquireWaitCount || !startStats.acquireWaitCount.W) {
            startStats.acquireWaitCount = {W: 0};
        }
        if (!startStats.timeAcquiringMicros || !startStats.timeAcquiringMicros.W) {
            startStats.timeAcquiringMicros = {W: 0};
        }

        var acquireWaitCount = endStats.acquireWaitCount.W - startStats.acquireWaitCount.W;
        var blockedMillis =
            Math.floor((endStats.timeAcquiringMicros.W - startStats.timeAcquiringMicros.W) / 1000);

        // Require that no other commands run (and maybe acquire locks) in parallel.
        assert.eq(acquireWaitCount, 1, "other commands ran in parallel, can't check timing");
        assert.gte(blockedMillis, minBlockedMillis, "reported time acquiring lock is too low");
        assert.lte(blockedMillis, maxBlockedMillis, "reported time acquiring lock is too high");
        return ({
            blockedMillis: blockedMillis,
            minBlockedMillis: minBlockedMillis,
            maxBlockedMillis: maxBlockedMillis
        });
    }

    var conn = MongoRunner.runMongod();
    var db = conn.getDB('test');
    printjson([1, 10, 100, 500, 1000, 1500].map(testBlockTime));
    MongoRunner.stopMongod(conn);
})();
