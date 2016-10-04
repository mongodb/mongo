// Test that the plan summary string appears in db.currentOp() for
// count operations. SERVER-14064.

var t = db.jstests_count_plan_summary;
t.drop();

for (var i = 0; i < 1000; i++) {
    t.insert({x: 1});
}

// Mock a long-running count operation by sleeping for each of
// the documents in the collection.
var awaitShell =
    startParallelShell("db.jstests_count_plan_summary.find({x: 1, $where: 'sleep(100)'}).count()");

// Find the count op in db.currentOp() and check for the plan summary.
assert.soon(function() {
    var current = db.currentOp({ns: t.getFullName(), "query.count": t.getName()});

    assert("inprog" in current);
    if (current.inprog.length === 0) {
        print("Did not find count op. db.currentOp() output:");
        printjson(current);
        return false;
    }

    // There are no indices, so the plan summary should be a collscan.
    var countOp = current.inprog[0];
    if (!("planSummary" in countOp)) {
        print("count op does not yet contain planSummary:");
        printjson(countOp);
        return false;
    }

    // There are no indices, so the planSummary should be "COLLSCAN".
    print("Found count op with planSummary:");
    printjson(countOp);
    assert.eq("COLLSCAN", countOp.planSummary, "wrong planSummary string");

    // Kill the op so that the test won't run for a long time.
    db.killOp(countOp.opid);

    return true;
});

var exitCode = awaitShell({checkExitSuccess: false});
assert.neq(0, exitCode, "expected shell to exit abnormally due to JS execution being terminated");
