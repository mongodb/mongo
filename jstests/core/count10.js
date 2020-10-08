// Test that interrupting a count returns an error code.
//
// @tags: [
//   # This test attempts to perform a count command and find it using the currentOp command. The
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

(function() {
"use strict";

var coll = db.count10;
coll.drop();

var bulk = coll.initializeUnorderedBulkOp();
for (var i = 0; i < 100; i++) {
    coll.insertOne({x: i});
}
assert.commandWorked(bulk.execute());

// Start a parallel shell which repeatedly checks for a count
// query using db.currentOp(). As soon as the op is found,
// kill it via db.killOp().
var s = startParallelShell(function() {
    assert.soon(function() {
        var currentCountOps =
            db.getSiblingDB("admin")
                .aggregate([
                    {$currentOp: {}},
                    {
                        $match:
                            {"ns": db.count10.getFullName(), "command.count": db.count10.getName()}
                    },
                    {$limit: 1}
                ])
                .toArray();

        // Check that we found the count op. If not, return false so
        // that assert.soon will retry.
        if (currentCountOps.length !== 1) {
            jsTest.log("Still didn't find count operation in the currentOp log.");
            return false;
        }

        var countOp = currentCountOps[0];
        jsTest.log("Found count op:");
        printjson(countOp);
        // Found the count op. Try to kill it.
        assert.commandWorked(db.killOp(countOp.opid));
        return true;
    }, "Could not find count op after retrying, gave up");
});

var res = assert.throws(function() {
    coll.find("sleep(1000)").count();
}, [], "Count op completed without being killed");

jsTest.log("Killed count output start");
printjson(res);
jsTest.log("Killed count output end");
assert(res.message.match(/count failed/) !== null);
assert(res.message.match(/\"code\"/) !== null);

s();
})();
