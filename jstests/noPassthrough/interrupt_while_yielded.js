(function() {
"use strict";

const kFailPointName = "setYieldAllLocksHang";
const kCommandComment = "interruptedWhileYieldedComment";
const kUseSbe = true;

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");
const coll = db.interrupt_while_yielded;

coll.drop();
assert.commandWorked(coll.insert({a: 1, b: 1, c: 1}));
assert.commandWorked(coll.insert({a: 1, b: 1, c: 1}));

assert.commandWorked(coll.createIndex({a: 1, b: 1}));
assert.commandWorked(coll.createIndex({a: 1, c: 1}));

// This is needed to make sure that a yield point is reached.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));

/**
 * Executes 'queryFn' in a parallel shell while a failpoint is enabled to hang operations during
 * yield. Ensures that operation run by 'queryFn' reaches the yield point, then runs killOp()
 * against the yielded operation.
 *
 * The 'useSbe' boolean allows to caller to indicate whether the test should run using the classic
 * query execution engine or the slot-based execution engine.
 */
function runTestWithQuery(queryFn, useSbe = false) {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryEnableSlotBasedExecutionEngine: useSbe}));

    let waitForParallelShell = null;

    try {
        assert.commandWorked(db.adminCommand({
            configureFailPoint: kFailPointName,
            mode: "alwaysOn",
            data: {namespace: coll.getFullName(), checkForInterruptAfterHang: true}
        }));

        // Run a command that should hit the fail point in a parallel shell.
        let code = `let queryFn = ${queryFn};`;
        code += `const coll = db.${coll.getName()};`;
        code += `const kCommandComment = "${kCommandComment}";`;
        function parallelShellFn() {
            const err = assert.throws(queryFn);
            assert.commandFailedWithCode(err, [ErrorCodes.Interrupted]);
        }
        code += "(" + parallelShellFn.toString() + ")();";

        waitForParallelShell = startParallelShell(code, conn.port);

        // Find the operation running the query.
        let opId = null;

        assert.soon(function() {
            const ops = db.getSiblingDB("admin")
                            .aggregate([
                                {$currentOp: {allUsers: true, localOps: true}},
                                {
                                    $match: {
                                        numYields: {$gt: 0},
                                        ns: coll.getFullName(),
                                        "command.comment": kCommandComment
                                    }
                                }
                            ])
                            .toArray();

            if (ops.length > 0) {
                assert.eq(ops.length, 1);
                opId = ops[0].opid;
                return true;
            }

            return false;
        });

        // Kill the op.
        db.killOp(opId);

    } finally {
        // Disable the failpoint so that the server will continue, and hit an interrupt check.
        assert.commandWorked(db.adminCommand({configureFailPoint: kFailPointName, mode: "off"}));

        if (waitForParallelShell) {
            waitForParallelShell();
        }
    }

    // Check that the server is still up.
    assert.commandWorked(db.adminCommand({hello: 1}));
}

function rootedOr() {
    coll.find({$or: [{a: 1}, {b: 1}]}).comment(kCommandComment).itcount();
}
runTestWithQuery(rootedOr);
runTestWithQuery(rootedOr, kUseSbe);

function groupFindDistinct() {
    coll.aggregate([{$group: {_id: "$a"}}], {comment: kCommandComment}).itcount();
}
runTestWithQuery(groupFindDistinct);
runTestWithQuery(groupFindDistinct, kUseSbe);

function projectImmediatelyAfterMatch() {
    coll.aggregate([{$match: {a: 1}}, {$project: {_id: 0, a: 1}}, {$unwind: "$a"}],
                   {comment: kCommandComment})
        .itcount();
}
runTestWithQuery(projectImmediatelyAfterMatch);
runTestWithQuery(projectImmediatelyAfterMatch, kUseSbe);

function sortImmediatelyAfterMatch() {
    coll.aggregate([{$match: {a: 1, b: 1, c: 1}}, {$sort: {a: 1}}], {comment: kCommandComment})
        .itcount();
}
runTestWithQuery(sortImmediatelyAfterMatch);
runTestWithQuery(sortImmediatelyAfterMatch, kUseSbe);

function sortAndProjectionImmediatelyAfterMatch() {
    coll.aggregate([{$match: {a: 1}}, {$project: {_id: 0, a: 1}}, {$sort: {a: 1}}],
                   {comment: kCommandComment})
        .itcount();
}
runTestWithQuery(sortAndProjectionImmediatelyAfterMatch);
runTestWithQuery(sortAndProjectionImmediatelyAfterMatch, kUseSbe);

MongoRunner.stopMongod(conn);
}());
