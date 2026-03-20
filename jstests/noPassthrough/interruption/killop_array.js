/**
 * Tests that killOp accepts an array of opIds and kills all matching operations.
 */

const dbName = "killop_array";
const collName = "test";

const conn = MongoRunner.runMongod();

const primary = conn;
const testDB = primary.getDB(dbName);

assert.commandWorked(testDB.getCollection(collName).insert([{x: 1}, {x: 2}, {x: 3}]));
assert.commandWorked(primary.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));

// Helper to start a parallel shell running a long find with a given comment.
function startBlockedFind(comment) {
    const query =
        `assert.commandWorked(db.getSiblingDB('${dbName}').runCommand(` +
        `{find: '${collName}', filter: {x: 1}, comment: '${comment}'}));`;
    return startParallelShell(query, primary.port);
}

// Helper to wait until an op with the given comment appears in currentOp and has yielded.
function waitForOp(comment) {
    let opId;
    assert.soon(
        function () {
            const result = testDB.currentOp({"command.comment": comment});
            assert.commandWorked(result);
            if (result.inprog.length === 1 && result.inprog[0].numYields > 0) {
                opId = result.inprog[0].opid;
                return true;
            }
            return false;
        },
        () => "Timed out waiting for op with comment: " + comment + ". currentOp: " + tojson(testDB.currentOp()),
    );
    return opId;
}

// ---- Test 1: killOp with a single-element array ----
jsTestLog("Test 1: killOp with a single-element array");
assert.commandWorked(primary.adminCommand({"configureFailPoint": "setYieldAllLocksHang", "mode": "alwaysOn"}));

const shell1 = startBlockedFind("single_array_comment");
const opId1 = waitForOp("single_array_comment");

assert.commandWorked(primary.adminCommand({killOp: 1, op: [opId1]}));

let res = testDB.currentOp({"command.comment": "single_array_comment"});
assert.commandWorked(res);
assert.eq(1, res.inprog.length, "expected op to still be in currentOp");
assert.eq(true, res.inprog[0].killPending, "expected killPending to be set");

assert.commandWorked(primary.adminCommand({"configureFailPoint": "setYieldAllLocksHang", "mode": "off"}));
shell1({checkExitSuccess: false});

// ---- Test 2: killOp with a multi-element array kills all listed ops ----
jsTestLog("Test 2: killOp with a multi-element array");
assert.commandWorked(primary.adminCommand({"configureFailPoint": "setYieldAllLocksHang", "mode": "alwaysOn"}));

const shell2 = startBlockedFind("multi_array_comment_a");
const shell3 = startBlockedFind("multi_array_comment_b");
const opId2 = waitForOp("multi_array_comment_a");
const opId3 = waitForOp("multi_array_comment_b");

assert.commandWorked(primary.adminCommand({killOp: 1, op: [opId2, opId3]}));

res = testDB.currentOp({"command.comment": /multi_array_comment/});
assert.commandWorked(res);
assert.eq(2, res.inprog.length, "expected both ops to still be in currentOp");
for (const op of res.inprog) {
    assert.eq(true, op.killPending, "expected killPending to be set for op: " + tojson(op));
}

assert.commandWorked(primary.adminCommand({"configureFailPoint": "setYieldAllLocksHang", "mode": "off"}));
shell2({checkExitSuccess: false});
shell3({checkExitSuccess: false});

// ---- Test 3: killOp with a scalar still works ----
jsTestLog("Test 3: killOp with a scalar opId");
assert.commandWorked(primary.adminCommand({"configureFailPoint": "setYieldAllLocksHang", "mode": "alwaysOn"}));

const shell4 = startBlockedFind("scalar_comment");
const opId4 = waitForOp("scalar_comment");

assert.commandWorked(primary.adminCommand({killOp: 1, op: opId4}));

res = testDB.currentOp({"command.comment": "scalar_comment"});
assert.commandWorked(res);
assert.eq(1, res.inprog.length);
assert.eq(true, res.inprog[0].killPending);

assert.commandWorked(primary.adminCommand({"configureFailPoint": "setYieldAllLocksHang", "mode": "off"}));
shell4({checkExitSuccess: false});

// ---- Test 4: empty array is rejected ----
jsTestLog("Test 4: killOp with an empty array is rejected");
assert.commandFailed(primary.adminCommand({killOp: 1, op: []}));

// ---- Test 5: non-numeric elements in array are rejected ----
jsTestLog("Test 5: killOp with non-numeric element in array is rejected");
assert.commandFailed(primary.adminCommand({killOp: 1, op: [123, "not-a-number"]}));

jsTestLog("All tests passed.");
MongoRunner.stopMongod(conn);
