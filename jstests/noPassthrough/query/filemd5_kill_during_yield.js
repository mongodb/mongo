/**
 * Tests that interrupting a filemd5 command while the PlanExecutor is yielded will correctly clean
 * up the PlanExecutor without crashing the server. This test was designed to reproduce SERVER-35361
 * and SERVER-107784.
 * @tags: [
 *   requires_replication,
 * ]
 */

import {waitForCurOpByFailPoint} from "jstests/libs/curop_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kFailPointName = "waitInFilemd5DuringManualYield";

// Helper function to get admin DB for replica set operations
function getAdminDB(connection) {
    let adminDB;
    if (typeof connection.getDB === "function") {
        adminDB = connection.getDB("admin");
    } else {
        assert(typeof connection.getSiblingDB === "function", `Cannot get Admin DB from ${tojson(connection)}`);
        adminDB = connection.getSiblingDB("admin");
    }
    return adminDB;
}

function stepDown(connection) {
    jsTest.log.info("Force stepDown", {connection});
    const adminDB = getAdminDB(connection);
    assert.commandWorkedOrFailedWithCode(
        adminDB.runCommand({replSetStepDown: 10, force: true, secondaryCatchUpPeriodSecs: 8}),
        [ErrorCodes.HostUnreachable],
    );
    jsTest.log.info("Forced step down to:", {connection});
}

// Data Distribution: Set up fs.chunks collection with GridFS test data
function setupTestData(database) {
    database.fs.chunks.drop();
    assert.commandWorked(database.fs.chunks.insert({files_id: 1, n: 0, data: new BinData(0, "64string")}));
    assert.commandWorked(database.fs.chunks.insert({files_id: 1, n: 1, data: new BinData(0, "test")}));
    assert.commandWorked(database.fs.chunks.createIndex({files_id: 1, n: 1}));
}

jsTest.log.info("Test filemd5 command interruption with killOp on standalone mongod");
{
    const conn = MongoRunner.runMongod();
    assert.neq(null, conn);

    const db = conn.getDB("test");
    setupTestData(db);

    assert.commandWorked(db.adminCommand({configureFailPoint: kFailPointName, mode: "alwaysOn"}));

    const failingMD5Shell = startParallelShell(() => {
        assert.commandFailedWithCode(db.runCommand({filemd5: 1, root: "fs"}), ErrorCodes.Interrupted);
    }, conn.port);

    // Wait for filemd5 to manually yield and hang
    const curOps = waitForCurOpByFailPoint(db, "test.fs.chunks", kFailPointName, {"command.filemd5": 1});
    const opId = curOps[0].opid;

    // Kill the operation, then disable the failpoint so the command recognizes it's been killed
    assert.commandWorked(db.killOp(opId));
    assert.commandWorked(db.adminCommand({configureFailPoint: kFailPointName, mode: "off"}));

    failingMD5Shell();

    MongoRunner.stopMongod(conn);
}

jsTest.log.info("Test filemd5 command interruption with stepDown on replica set");
{
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    rst.awaitSecondaryNodes();

    let primary = rst.getPrimary();
    const db = primary.getDB("test");
    setupTestData(db);

    assert.commandWorked(db.adminCommand({configureFailPoint: kFailPointName, mode: "alwaysOn"}));

    function runFilemd5Command() {
        jsTest.log.info("Running filemd5 command in parallel shell to test yield behavior, DB NAME " + db.getName());
        const cmd = {filemd5: 1, root: "fs"};
        const res = db.runCommand(cmd);
        assert.commandWorked(res, `filemd5 command failed: ${tojson(res)}`);
    }

    const failingMD5Shell = startParallelShell(runFilemd5Command, primary.port);

    // Wait for filemd5 to manually yield and hang
    const curOps = waitForCurOpByFailPoint(db, "test.fs.chunks", kFailPointName, {"command.filemd5": 1});
    const opId = curOps[0].opid;

    // Step down instead of killing the operation - this should interrupt the command and yield the
    // resources
    jsTest.log.info("Stepping down with command in progress.");
    stepDown(primary);

    assert.commandWorked(db.adminCommand({configureFailPoint: kFailPointName, mode: "off"}));

    failingMD5Shell();

    rst.stopSet(null, true);
}
