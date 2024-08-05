/**
 * Tests that the shell helper db.currentOpCursor isn't constrained by the legacy currentOp server
 * command - ie. the result set isn't limited to 16MB and long operations aren't truncated.
 *
 * Note: On newer branches, this test contains additional cases for the currentOp command (without a
 * shell helper) and the $currentOp pipeline stage, which are not included here. Those cases would
 * behave unreliably because of SERVER-92284.
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

(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // for FixtureHelpers
load('jstests/libs/parallel_shell_helpers.js');

const coll = db.currentOp_cursor;
coll.drop();

for (let i = 0; i < 100; i++) {
    assert.commandWorked(coll.insert({val: 1}));
}

//
// Test that db.currentOpCursor() returns an iterable cursor.
//
const cursorFromCurrentOp = db.currentOpCursor();
assert(cursorFromCurrentOp.hasNext());
assert(cursorFromCurrentOp.next());

//
// Test that db.currentOp() returns an object in the expected format.
//
const currentOpRes = db.currentOp();
assert("inprog" in currentOpRes, "Result contains 'inprog' field");
assert("ok" in currentOpRes, "Result contains 'ok' field");

//
// Test that attempting to access the fsyncLock field from the results throws with an error message.
//
const error = assert.throws(() => currentOpRes.fsyncLock);
assert(
    /fsyncLock is no longer included in the currentOp shell helper, run db\.runCommand\({currentOp: 1}\) instead/
        .test(error));

//
// Start a pipeline with a large command object in a parallel shell and then test three different
// methods of executing "currentOp" queries to ensure that they all observe the operation and that
// they do or do not truncate its command object (according to each one's specification).
//

// Starts the query. Intended to b e called from a parallel shell.
function startLongRunningAggregation(collName, comment) {
    function createLargeDoc() {
        let doc = {};
        for (let i = 0; i < 100; i++) {
            doc[i] = "Testing testing 1 2 3...";
        }
        return doc;
    }

    assert.commandFailedWithCode(db.runCommand({
        aggregate: collName,
        pipeline: [{
            $addFields: {
                newVal: {$function: {args: [], body: "sleep(1000000)", lang: "js"}},
                bigDoc: createLargeDoc()
            }
        }],
        comment: comment,
        cursor: {}
    }),
                                 ErrorCodes.Interrupted);
}

// Repeatedly executes 'getOperationsFunction()' until it returns exactly one operation for each
// shard in a sharded collection or exactly one operation for an unsharded collection.
function awaitOperations(getOperationsFunction) {
    let operations;
    assert.soon(
        function() {
            const numShards = FixtureHelpers.numberOfShardsForCollection(coll);
            operations = getOperationsFunction();

            // No shard should have more than one operation matching the query comment. First check
            // that the total number of operations is no greater than the total number of shards.
            assert.lte(operations.length, numShards, operations);

            // Also explicitly check that each shard appears no more than once in the list of
            // operations.
            const distinctShardNames = new Set(operations.map(op => "shard" in op ? op.shard : ""));
            assert.eq(operations.length, distinctShardNames.size, {operations, numShards});

            if (operations.length < numShards) {
                print(`Found ${operations.length} operation(s); waiting until there are ${
                    numShards} operation(s)`);
                return false;
            } else if (operations.some(op => op.op !== "getmore" && "cursor" in op &&
                                           op.cursor.batchSize === 0)) {
                print(`Found command with empty 'batchSize' value; waiting for getmore: ${
                    tojson(operations)}`);
                return false;
            }

            return true;
        },
        function() {
            return "Failed to find parallel shell operation in $currentOp output: " +
                tojson(db.currentOp());
        });

    return operations;
}

function getCommandFromCurrentOpEntry(entry) {
    if (entry.op === "command" && "command" in entry) {
        return entry.command;
    } else if (entry.op === "getmore" && "cursor" in entry &&
               "originatingCommand" in entry.cursor) {
        return entry.cursor.originatingCommand;
    } else {
        assert(false, entry);
    }
}

const comment = "long_running_aggregation";
const awaitShell =
    startParallelShell(funWithArgs(startLongRunningAggregation, coll.getName(), comment));

const filter = {
    ns: coll.getFullName(),
    "command.comment": comment,

    // On the replica set endpoint, currentOp reports both router and shard operations. So filter
    // out one of them.
    role: TestData.testingReplicaSetEndpoint ? "ClusterRole{router}" : {$exists: false}
};

// The 'currentOp' shell helper should _not_ truncate the command.
const operationsViaCurrentOpShellHelper = awaitOperations(function() {
    return db.currentOp(filter).inprog;
});
assert(operationsViaCurrentOpShellHelper.every(op => {
    const command = getCommandFromCurrentOpEntry(op);
    return !("$truncated" in command) && command.aggregate == coll.getName();
}),
       operationsViaCurrentOpShellHelper);

// Finish the test by killing the long-running aggregation pipeline and joining the parallel shell
// that launched it.
for (let op of operationsViaCurrentOpShellHelper) {
    assert.commandWorked(db.killOp(op.opid));
}
awaitShell();
})();
