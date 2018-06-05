// Test what happens when javascript execution inside the group command is interrupted, either from
// killOp, or due to timeout.
(function() {
    const conn = MongoRunner.runMongod({});
    assert.neq(null, conn);

    const db = conn.getDB("test");
    const coll = db.group_with_stepdown;
    const kFailPointName = "hangInGroupReduceJs";

    assert.writeOK(coll.insert({name: "bob", foo: 1}));
    assert.writeOK(coll.insert({name: "alice", foo: 1}));
    assert.writeOK(coll.insert({name: "fred", foo: 3}));
    assert.writeOK(coll.insert({name: "fred", foo: 4}));

    // Attempts to run the group command while the given failpoint is enabled.
    let awaitShellFn = null;
    try {
        assert.commandWorked(
            db.adminCommand({configureFailPoint: kFailPointName, mode: "alwaysOn"}));

        // Run a group in the background that will hang.
        function runHangingGroup() {
            const coll = db.group_with_stepdown;

            const err = assert.throws(() => coll.group({
                key: {foo: 1},
                initial: {count: 0},
                reduce: function(obj, prev) {
                    while (1) {
                        sleep(1000);
                    }
                }
            }),
                                      [],
                                      "expected group() to fail");

            assert.eq(err.code, ErrorCodes.Interrupted);
        }
        awaitShellFn = startParallelShell(runHangingGroup, conn.port);

        // Wait until we know the failpoint has been reached.
        let opid = null;
        assert.soon(function() {
            const arr = db.getSiblingDB("admin")
                            .aggregate([{$currentOp: {}}, {$match: {"msg": kFailPointName}}])
                            .toArray();

            if (arr.length == 0) {
                return false;
            }

            // Should never have more than one operation stuck on the failpoint.
            assert.eq(arr.length, 1);
            opid = arr[0].opid;
            return true;
        });

        // Kill the group().
        assert.neq(opid, null);
        assert.commandWorked(db.killOp(opid));
    } finally {
        assert.commandWorked(db.adminCommand({configureFailPoint: kFailPointName, mode: "off"}));

        if (awaitShellFn) {
            awaitShellFn();
        }
    }

    assert.eq(0, MongoRunner.stopMongod(conn), "expected mongod to shutdown cleanly");
})();
