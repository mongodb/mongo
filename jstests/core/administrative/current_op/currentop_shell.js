/**
 * Tests that the shell helper db.currentOpCursor isn't constrained by the legacy currentOp server
 * command - ie. the result set isn't limited to 16MB and long operations aren't truncated.
 *
 * @tags: [
 *   # The collection may be completely moved to another shard, which results in currentOp not
 *   # returning the expected command.
 *   assumes_balancer_off,
 *   # The test runs commands that are not allowed with security token: getLog.
 *   not_allowed_with_signed_security_token,
 *   uses_parallel_shell,
 *   # This test uses currentOp to check whether an aggregate command is running. In replica set
 *   # environments, because currentOp is run against the admin database it is routed to the
 *   # primary, while the aggregate may be routed to a secondary. If currentOp is running on one
 *   # node and the expected command is run on another, the latter will not show up in the
 *   # currentOp results.
 *   assumes_read_preference_unchanged,
 *   no_selinux,
 *   # Uses $function operator.
 *   requires_scripting,
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const coll = db.currentOp_cursor;
coll.drop();

for (let i = 0; i < 100; i++) {
    assert.commandWorked(coll.insert({val: 1}));
}

// Test that db.currentOpCursor() returns an iterable cursor.
let res = db.currentOpCursor();
assert(res.hasNext());
assert(res.next());

// Test that db.currentOp() interface does not change.
res = db.currentOp();
assert("inprog" in res, "Result contains 'inprog' field");
assert("ok" in res, "Result contains 'ok' field");

// Attempting to access the fsyncLock field from the results throws with an error message.
let error = assert.throws(() => res.fsyncLock);
assert(
    /fsyncLock is no longer included in the currentOp shell helper, run db\.runCommand\({currentOp: 1}\) instead/
        .test(error));

function shellOp() {
    function createLargeDoc() {
        let doc = {};
        for (let i = 0; i < 100; i++) {
            doc[i] = "Testing testing 1 2 3...";
        }
        return doc;
    }

    assert.commandFailedWithCode(db.runCommand({
        aggregate: "currentOp_cursor",
        pipeline: [{
            $addFields: {
                newVal: {$function: {args: [], body: "sleep(1000000)", lang: "js"}},
                bigDoc: createLargeDoc()
            }
        }],
        comment: TestData.comment,
        cursor: {}
    }),
                                 ErrorCodes.Interrupted);
}

function startShellWithOp(comment) {
    TestData.comment = comment;
    const awaitShell = startParallelShell(shellOp);

    // Confirm that the operation has started in the parallel shell.
    assert.soon(
        function() {
            let shards = FixtureHelpers.numberOfShardsForCollection(coll);
            let aggRes =
                db.getSiblingDB("admin")
                    .aggregate([
                        {$currentOp: {}},
                        {$match: {ns: "test.currentOp_cursor", "command.comment": TestData.comment}}
                    ])
                    .toArray();
            return aggRes.length >= shards;
        },
        function() {
            return "Failed to find parallel shell operation in $currentOp output: " +
                tojson(db.currentOp());
        });
    return awaitShell;
}

/**
 * Tests the currentOp behaviour by first starting a parallel shell, where a long running command is
 * being executed. After that a 'commandFn' is called, which is retrieving currentOps either through
 * a dedicated command or a shell extension function.
 */
function testCurrentOp(comment, commandFn, shouldTruncate) {
    const serverCommandTest = startShellWithOp(comment);
    const res = commandFn({
        $and: [
            {"ns": "test.currentOp_cursor"},
            {"command.comment": comment},
            // On the replica set endpoint, currentOp reports both router and shard
            // operations. So filter out one of them.
            TestData.testingReplicaSetEndpoint ? {role: "ClusterRole{router}"}
                                               : {role: {$exists: false}}
        ]
    });
    assert.eq(res.inprog.length, FixtureHelpers.numberOfShardsForCollection(coll), res);
    res.inprog.forEach((result) => {
        if (result.op === 'command') {
            assert.eq(shouldTruncate, result.command.hasOwnProperty("$truncated"), res);
        } else {
            assert.eq(result.op, 'getmore', res);
            assert.eq(
                shouldTruncate, result.cursor.originatingCommand.hasOwnProperty("$truncated"), res);
        }
    });

    res.inprog.forEach((op) => {
        assert.commandWorked(db.killOp(op.opid));
    });

    serverCommandTest();
}

testCurrentOp("currentOp_server",
              (args) => db.adminCommand({currentOp: true, ...args}),
              true /* shouldTruncate */);

testCurrentOp("currentOp_shell", (args) => db.currentOp(args), false /* shouldTruncate */);
