// Tests that planSummary is not duplicated in an active getmore currentOp entry.
(function() {
    "use strict";

    // This test runs a getMore in a parallel shell, which will not inherit the implicit session of
    // the cursor establishing command.
    TestData.disableImplicitSessions = true;

    const collName = "currentop_plan_summary_no_dup";
    const coll = db.getCollection(collName);
    coll.drop();
    for (let i = 0; i < 200; i++) {
        assert.commandWorked(coll.insert({x: 1}));
    }

    // Create a long-running getMore operation by sleeping for every document.
    const cmdRes = assert.commandWorked(db.runCommand({
        find: collName,
        filter: {
            $where: function() {
                sleep(100);
                return true;
            }
        },
        batchSize: 0
    }));
    const cmdStr = 'db.runCommand({getMore: ' + cmdRes.cursor.id.toString() + ', collection: "' +
        collName + '"})';
    const awaitShell = startParallelShell(cmdStr);

    assert.soon(function() {
        const currOp = db.currentOp({"op": "getmore"});

        assert("inprog" in currOp);
        if (currOp.inprog.length === 0) {
            return false;
        }

        const getmoreOp = currOp.inprog[0];
        if (!("planSummary" in getmoreOp)) {
            print("getMore op does not yet contain planSummary:");
            printjson(getmoreOp);
            return false;
        }

        // getmoreOp should only contain a top-level plan summary.
        // Check that it doesn't contain a sub-level duplicate.
        assert(!getmoreOp.cursor.hasOwnProperty("planSummary"),
               "getmore contains duplicated planSummary: " + tojson(getmoreOp));

        // Kill the op so that the test won't run for a long time.
        db.killOp(getmoreOp.opid);

        return true;
    });
    awaitShell();
}());
