/**
 * Shared test logic for logical session cache refresh serialization tests.
 *
 * Callers are responsible for starting and stopping the replica set fixture; this module
 * only exercises the session refresh behavior against an already-running primary.
 */

export var LogicalSessionCacheRefreshSerializationTest = (function () {
    /**
     * Runs the logical session cache refresh serialization test against `primary`.
     *
     * Verifies two things:
     *   1. Sessions are eventually written to config.system.sessions without an explicit
     *      refresh command (relying on logicalSessionRefreshMillis).
     *   2. After calling refreshLogicalSessionCacheNow, a session is immediately visible
     *      in the sessions collection.
     */
    function run(primary) {
        const adminDB = primary.getDB("admin");
        const sessionsColl = primary.getDB("config").system.sessions;

        // Sanity check: a session should appear in the cache once the refresh interval elapses,
        // without an explicit refreshLogicalSessionCacheNow call.
        {
            const session = primary.startSession();
            const sessionId = session.getSessionId();
            assert.commandWorked(session.getDatabase("test").runCommand({insert: "coll", documents: [{_id: 1000}]}));

            jsTest.log.info("Waiting for session to be added to the cache after logicalSessionRefreshMillis elapses", {
                sessionId,
            });
            assert.soon(
                () => sessionsColl.find({"_id.id": sessionId.id}).itcount() === 1,
                "Session should be present after logicalSessionRefreshMillis elapses",
            );

            session.endSession();
        }

        const numIterations = 10;
        const sessions = [];
        for (let i = 0; i < numIterations; i++) {
            jsTest.log.info(`Starting session ${i} and using it to add to the cache`);
            const session = primary.startSession();
            sessions.push(session);
            const sessionId = session.getSessionId();
            assert.commandWorked(session.getDatabase("test").runCommand({insert: "coll", documents: [{_id: i}]}));

            jsTest.log.info(`Call refreshLogicalSessionCacheNow iteration ${i}`);
            assert.commandWorked(adminDB.runCommand({refreshLogicalSessionCacheNow: 1}));
            assert.eq(
                1,
                sessionsColl.find({"_id.id": sessionId.id}).itcount(),
                "Session should be present after refreshLogicalSessionCacheNow finishes",
            );
        }
        for (const session of sessions) {
            session.endSession();
        }
    }

    return {run};
})();
