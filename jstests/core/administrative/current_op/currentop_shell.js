/**
 * Tests that the shell helper db.currentOpCursor isn't constrained by the legacy currentOp server
 * command - ie. the result set isn't limited to 16MB and long operations aren't truncated.
 *
 * @tags: [
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
            let aggRes =
                db.getSiblingDB("admin")
                    .aggregate([
                        {$currentOp: {}},
                        {$match: {ns: "test.currentOp_cursor", "command.comment": TestData.comment}}
                    ])
                    .toArray();
            return aggRes.length >= 1;
        },
        function() {
            return "Failed to find parallel shell operation in $currentOp output: " +
                tojson(db.currentOp());
        });
    return awaitShell;
}

// Test that the currentOp server command truncates long operations with a warning logged.
const serverCommandTest = startShellWithOp("currentOp_server");
res = db.adminCommand({
    currentOp: true,
    $and: [{"ns": "test.currentOp_cursor"}, {"command.comment": "currentOp_server"}]
});

if (FixtureHelpers.isMongos(db) && FixtureHelpers.isSharded(coll)) {
    // Assert currentOp truncation behavior for each shard in the cluster.
    assert(res.inprog.length >= 1, res);
    res.inprog.forEach((result) => {
        assert.eq(result.op, "getmore", res);
        assert(result.cursor.originatingCommand.hasOwnProperty("$truncated"), res);
    });
} else {
    // Assert currentOp truncation behavior for unsharded collections.
    assert.eq(res.inprog.length, 1, res);
    assert.eq(res.inprog[0].op, "command", res);
    assert(res.inprog[0].command.hasOwnProperty("$truncated"), res);
}

const log = FixtureHelpers.getPrimaryForNodeHostingDatabase(db).adminCommand({getLog: "global"});
assert(/will be truncated/.test(log.log));

res.inprog.forEach((op) => {
    assert.commandWorked(db.killOp(op.opid));
});

serverCommandTest();

// Test that the db.currentOp() shell helper does not truncate ops.
const shellHelperTest = startShellWithOp("currentOp_shell");
res = db.currentOp({
    $and: [
        {"ns": "test.currentOp_cursor"},
        {"command.comment": "currentOp_shell"},
        // On the replica set endpoint, currentOp reports both router and shard operations. So
        // filter out one of them.
        TestData.testingReplicaSetEndpoint ? {role: "ClusterRole{router}"}
                                           : {role: {$exists: false}}
    ]
});

if (FixtureHelpers.isMongos(db) && FixtureHelpers.isSharded(coll)) {
    assert(res.inprog.length >= 1, res);
    res.inprog.forEach((result) => {
        assert.eq(result.op, "getmore", res);
        assert(!result.cursor.originatingCommand.hasOwnProperty("$truncated"), res);
    });
} else {
    assert.eq(res.inprog.length, 1, res);
    assert(!res.inprog[0].command.hasOwnProperty("$truncated"), res);
}

res.inprog.forEach((op) => {
    assert.commandWorked(db.killOp(op.opid));
});

shellHelperTest();
