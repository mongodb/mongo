// Test that the plan summary string appears in db.currentOp() for count operations. SERVER-14064.
//
// @tags: [
//   # This test attempts to perform a find command and find it using the currentOp command. The
//   # former operation may be routed to a secondary in the replica set, whereas the latter must be
//   # routed to the primary.
//   assumes_read_preference_unchanged,
//   # The aggregation stage $currentOp cannot run with a readConcern other than 'local'
//   assumes_read_concern_unchanged,
//   does_not_support_repeated_reads,
//   # Uses $where operator
//   requires_scripting,
//   uses_multiple_connections,
//   uses_parallel_shell,
//   requires_getmore,
//   # The count operation can't be retried, otherwise we may get a timeout waiting for the parallel
//   # shell to finish.
//   does_not_support_stepdowns,
//   assumes_balancer_off,
// ]
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

let t = db.jstests_count_plan_summary;
t.drop();
t.insert({x: 1});

function setPostCommandFailpoint({mode, options}) {
    FixtureHelpers.runCommandOnEachPrimary({
        db: db.getSiblingDB("admin"),
        cmdObj: {configureFailPoint: "waitAfterCommandFinishesExecution", data: options, mode: mode},
    });
}

setPostCommandFailpoint({mode: "alwaysOn", options: {commands: ["count"]}});
let awaitShell = startParallelShell(() => {
    jsTest.log("Starting long-running count in parallel shell");
    db.jstests_count_plan_summary.find({x: 1}).count();
    jsTest.log("Finished long-running count in parallel shell");
});
try {
    // Find the count op in db.currentOp() and check for the plan summary.
    assert.soon(function () {
        let currentCountOps = db
            .getSiblingDB("admin")
            .aggregate([
                {$currentOp: {}},
                {
                    $match: {
                        $and: [
                            {"command.count": t.getName()},
                            // On the replica set endpoint, currentOp reports both router and
                            // shard operations. So filter out one of them.
                            TestData.testingReplicaSetEndpoint
                                ? {role: "ClusterRole{shard}"}
                                : {role: {$exists: false}},
                        ],
                    },
                },
                {$limit: 1},
            ])
            .toArray();

        if (currentCountOps.length !== 1) {
            jsTest.log("Still didn't find count operation in the currentOp log.");
            return false;
        }

        let countOp = currentCountOps[0];
        if (!("planSummary" in countOp)) {
            jsTest.log.info("Count op does not yet contain planSummary.", {countOp});
            return false;
        }

        // There are no indices, so the planSummary should be "COLLSCAN".
        jsTest.log("Found count op with planSummary:");
        printjson(countOp);
        assert.eq("COLLSCAN", countOp.planSummary, "wrong planSummary string");

        return true;
    }, "Did not find count operation in current operation log");
} finally {
    setPostCommandFailpoint({mode: "off", options: {}});
}
let exitCode = awaitShell();
