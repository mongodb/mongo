// Tests that specifying a maxTimeMS on a getMore for a tailable + awaitData cursor is not
// interpreted as a deadline for the operation.
// This test was designed to reproduce SERVER-33942 against a mongod.
// @tags: [requires_capped]
(function() {
    "use strict";

    // This test runs a getMore in a parallel shell, which will not inherit the implicit session of
    // the cursor establishing command.
    TestData.disableImplicitSessions = true;

    const coll = db.tailable_getmore_no_timeout;
    coll.drop();

    assert.commandWorked(db.runCommand({create: coll.getName(), capped: true, size: 1024}));

    for (let i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({_id: i}));
    }

    const findResponse = assert.commandWorked(
        db.runCommand({find: coll.getName(), filter: {}, tailable: true, awaitData: true}));
    const cursorId = findResponse.cursor.id;
    assert.neq(0, cursorId);

    // Start an operation in a parallel shell that holds the lock for a while.
    const awaitSleepShell = startParallelShell(
        () => assert.commandFailedWithCode(db.adminCommand({sleep: 1, lock: "w", secs: 600}),
                                           ErrorCodes.Interrupted));

    // Start a getMore and verify that it is waiting for the lock.
    const getMoreMaxTimeMS = 10;
    const awaitGetMoreShell = startParallelShell(`
        // Wait for the sleep command to take the lock.
        assert.soon(() => db.getSiblingDB("admin")
                              .currentOp({"command.sleep": 1, active: true})
                              .inprog.length === 1);
        // Start the getMore with a low maxTimeMS.
        assert.commandWorked(db.runCommand({
            getMore: ${cursorId.toString()},
            collection: "${coll.getName()}",
            maxTimeMS: ${getMoreMaxTimeMS}
        }));
    `);

    // Wait to see the getMore waiting on the lock.
    assert.soon(
        () =>
            db.currentOp({"command.getMore": cursorId, waitingForLock: true}).inprog.length === 1);

    // Sleep for twice the maxTimeMS to prove that the getMore won't time out waiting for the lock.
    sleep(2 * getMoreMaxTimeMS);

    // Then kill the command with the lock, allowing the getMore to continue successfully.
    const sleepOps = db.getSiblingDB("admin").currentOp({"command.sleep": 1, active: true}).inprog;
    assert.eq(sleepOps.length, 1);
    const sleepOpId = sleepOps[0].opid;
    assert.commandWorked(db.adminCommand({killOp: 1, op: sleepOpId}));

    awaitSleepShell();
    awaitGetMoreShell();
}());
