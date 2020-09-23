// Test that the plan summary string appears in db.currentOp() for count operations. SERVER-14064.
//
// @tags: [
//   # This test attempts to perform a find command and find it using the currentOp command. The
//   # former operation may be routed to a secondary in the replica set, whereas the latter must be
//   # routed to the primary.
//   assumes_read_preference_unchanged,
//   # The aggregation stage $currentOp cannot run with a readConcern other than 'local'
//   assumes_read_concern_unchanged,
//   does_not_support_stepdowns,
//   # Uses $where operator
//   requires_scripting,
//   uses_multiple_connections,
//   uses_parallel_shell,
// ]

var t = db.jstests_count_plan_summary;
t.drop();

for (var i = 0; i < 1000; i++) {
    t.insert({x: 1});
}

// Mock a long-running count operation by sleeping for each of
// the documents in the collection.
var awaitShell = startParallelShell(() => {
    jsTest.log("Starting long-running count in parallel shell");
    db.jstests_count_plan_summary.find({x: 1, $where: 'sleep(100); return true;'}).count();
    jsTest.log("Finished long-running count in parallel shell");
});

// Find the count op in db.currentOp() and check for the plan summary.
assert.soon(function() {
    var currentCountOps =
        db.getSiblingDB("admin")
            .aggregate([{$currentOp: {}}, {$match: {"command.count": t.getName()}}, {$limit: 1}])
            .toArray();

    if (currentCountOps.length !== 1) {
        jsTest.log("Still didn't find count operation in the currentOp log.");
        return false;
    }

    var countOp = currentCountOps[0];
    if (!("planSummary" in countOp)) {
        jsTest.log("Count op does not yet contain planSummary.");
        printjson(countOp);
        return false;
    }

    // There are no indices, so the planSummary should be "COLLSCAN".
    jsTest.log("Found count op with planSummary:");
    printjson(countOp);
    assert.eq("COLLSCAN", countOp.planSummary, "wrong planSummary string");

    // Kill the op so that the test won't run for a long time.
    db.killOp(countOp.opid);

    return true;
}, "Did not find count operation in current operation log");

var exitCode = awaitShell({checkExitSuccess: false});
assert.neq(0, exitCode, "Expected shell to exit abnormally due to JS execution being terminated");
