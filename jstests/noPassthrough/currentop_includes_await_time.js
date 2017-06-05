/**
 * Test that the operation latencies reported in current op for a getMore on an awaitData cursor
 * include time spent blocking for the await time.
 */
(function() {
    "use test";

    const conn = MongoRunner.runMongod({});
    assert.neq(null, conn, "mongod was unable to start up");
    const testDB = conn.getDB("test");
    const coll = testDB.currentop_includes_await_time;

    coll.drop();
    assert.commandWorked(testDB.createCollection(coll.getName(), {capped: true, size: 1024}));
    assert.writeOK(coll.insert({_id: 1}));

    let cmdRes = assert.commandWorked(
        testDB.runCommand({find: coll.getName(), tailable: true, awaitData: true}));

    TestData.commandResult = cmdRes;
    let cleanupShell = startParallelShell(function() {
        db.getSiblingDB("test").runCommand({
            getMore: TestData.commandResult.cursor.id,
            collection: "currentop_includes_await_time",
            maxTimeMS: 5 * 60 * 1000,
        });
    }, conn.port);

    assert.soon(function() {
        // This filter ensures that the getMore 'secs_running' and 'microsecs_running' fields are
        // sufficiently large that they appear to include time spent blocking waiting for capped
        // inserts.
        let ops = testDB.currentOp({
            "command.getMore": {$exists: true},
            "ns": coll.getFullName(),
            secs_running: {$gte: 2},
            microsecs_running: {$gte: 2 * 1000 * 1000}
        });
        return ops.inprog.length === 1;
    }, printjson(testDB.currentOp()));

    // A capped insertion should unblock the getMore, allowing the test to complete before the
    // getMore's awaitData time expires.
    assert.writeOK(coll.insert({_id: 2}));

    cleanupShell();
}());
