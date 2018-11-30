/**
 * Test the situation where
 * - A stepdown happens at the end of a getMore operation, after it has checked for interrupt the
     last time. The command will fail because the connection with the client will be cut as part of
     the stepdown, but the cursor will still exist.
 * - Kill the cursor using OP_KILL_CURSORS.
 *
 * This is intended to reproduce SERVER-37838.
 * @tags: [requires_replication]
 */
(function() {
    "use strict";

    load("jstests/libs/fixture_helpers.js");  // For waitUntilServerHangsOnFailPoint().

    // This test runs manual getMores using different connections, which will not inherit the
    // implicit session of the cursor establishing command.
    TestData.disableImplicitSessions = true;

    const kName = "stepdown_during_getmore_then_legacy_killop";
    const rst = new ReplSetTest({name: kName, nodes: 2});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const testDB = primary.getDB("test");
    const coll = testDB[kName];

    for (let i = 0; i < 10; i++) {
        assert.commandWorked(coll.insert({x: 1}));
    }
    rst.awaitReplication();

    // Start an aggregation.
    const csCmdRes = assert.commandWorked(
        coll.runCommand({aggregate: coll.getName(), pipeline: [], cursor: {batchSize: 2}}));

    const cursorId = csCmdRes.cursor.id;

    // Start a getMore that waits on a failpoint which comes after any checks for interrupt. This
    // ensures that once we kill the operation (by causing a stepdown), it will still succeed,
    // since it will have no opportunity to notice it's been killed.
    const kFailPointName = "waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch";
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: kFailPointName, mode: 'alwaysOn'}));

    let code = `const collName = "${coll.getName()}";`;
    code += `const cursorId = ${cursorId};`;
    function runGetMore() {
        const getMoreCmd = {getMore: cursorId, collection: collName, batchSize: 2};

        const exn = assert.throws(() => db[collName].runCommand(getMoreCmd));
        assert.gte(exn.message.indexOf("network error"), 0);
    }
    code += `(${runGetMore.toString()})();`;
    const getMoreJoiner = startParallelShell(code, primary.port);

    // Be sure the server is hanging on the failpoint.
    assert.soon(function() {
        const filter = {"msg": kFailPointName, "cursor.cursorId": cursorId};
        const ops = primary.getDB("admin")
                        .aggregate([{$currentOp: {allUsers: true}}, {$match: filter}])
                        .toArray();
        return ops.length == 1;
    });

    // Now step down the node. This will cause the getMore operation to be marked as killed, though
    // it will never check for interrupt because it's hanging on a failpoint that's past any
    // interrupt checks. When the operation is done using its cursor, it will realize that its been
    // killed and mark the _cursor_ as having been killed, so that further attempts to use it will
    // fail.
    assert.throws(function() {
        primary.adminCommand({replSetStepDown: ReplSetTest.kDefaultTimeoutMS, force: true});
    });

    assert.commandWorked(primary.adminCommand({configureFailPoint: kFailPointName, mode: 'off'}));
    getMoreJoiner();

    // Close the cursor with an OP_KILL_CURSORS.
    (function() {
        testDB.getMongo().forceReadMode("legacy");

        const curs = new DBCommandCursor(testDB, csCmdRes);
        curs.close();

        testDB.getMongo().forceReadMode("commands");
    })();

    rst.stopSet();
})();
