// Tests that interrupting a filemd5 command while the PlanExecutor is yielded will correctly clean
// up the PlanExecutor without crashing the server. This test was designed to reproduce
// SERVER-35361.
(function() {
    "use strict";

    const conn = MongoRunner.runMongod();
    assert.neq(null, conn);
    const db = conn.getDB("test");
    db.fs.chunks.drop();
    assert.writeOK(db.fs.chunks.insert({files_id: 1, n: 0, data: new BinData(0, "64string")}));
    assert.writeOK(db.fs.chunks.insert({files_id: 1, n: 1, data: new BinData(0, "test")}));
    db.fs.chunks.ensureIndex({files_id: 1, n: 1});

    const kFailPointName = "waitInFilemd5DuringManualYield";
    assert.commandWorked(db.adminCommand({configureFailPoint: kFailPointName, mode: "alwaysOn"}));

    const failingMD5Shell =
        startParallelShell(() => assert.commandFailedWithCode(
                               db.runCommand({filemd5: 1, root: "fs"}), ErrorCodes.Interrupted),
                           conn.port);

    // Wait for filemd5 to manually yield and hang.
    let opId;
    assert.soon(
        () => {
            const filter = {ns: "test.fs.chunks", "command.filemd5": 1, msg: kFailPointName};
            const result =
                db.getSiblingDB("admin").aggregate([{$currentOp: {}}, {$match: filter}]).toArray();

            if (result.length === 1) {
                opId = result[0].opid;

                return true;
            }

            return false;
        },
        () => "Failed to find operation in currentOp() output: " +
            tojson(db.currentOp({"ns": coll.getFullName()})));

    // Kill the operation, then disable the failpoint so the command recognizes it's been killed.
    assert.commandWorked(db.killOp(opId));
    assert.commandWorked(db.adminCommand({configureFailPoint: kFailPointName, mode: "off"}));

    failingMD5Shell();
    MongoRunner.stopMongod(conn);
}());
