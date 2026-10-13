/**
 * Tests that the 'newTermStartDate' and 'wMajorityWriteAvailabilityDate' fields of the
 * replSetGetStatus 'electionCandidateMetrics' section are present only when they should be.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartReplSetReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

const name = jsTestName();

// Older binaries used in multiversion tests do not have this failpoint.
const isMultiversion =
    Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet) ||
    Boolean(TestData.multiversionBinVersion);

const rstOptions = {name: name, nodes: 3};
if (!isMultiversion) {
    // Force off replicated fast count so the test exercises the legacy size/count tracking path.
    rstOptions.nodeOptions = {
        setParameter: {
            "failpoint.disableReplicatedFastCount": tojson({mode: "alwaysOn"}),
        },
    };
}
const rst = new ReplSetTest(rstOptions);

rst.startSet();
rst.initiate();
rst.awaitReplication();

// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(
    rst.getPrimary().adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {w: 1},
        writeConcern: {w: "majority"},
    }),
);
rst.awaitReplication();
stopServerReplication(rst.nodes);

// Step up one of the secondaries.
const newPrimary = rst.getSecondary();
assert.soonNoExcept(
    function () {
        assert.commandWorked(newPrimary.adminCommand({replSetStepUp: 1}));
        rst.awaitNodesAgreeOnPrimary(rst.timeoutMS, rst.nodes, newPrimary);
        return newPrimary.adminCommand("replSetGetStatus").myState === ReplSetTest.State.PRIMARY;
    },
    "failed to step up node " + newPrimary.host,
    rst.timeoutMS,
);

// Wait until the new primary completes the transition to primary and writes a no-op.
assert.eq(rst.getPrimary(), newPrimary);

// Check that the 'electionCandidateMetrics' section of the replSetGetStatus response has the
// 'newTermStartDate' field once the transition to primary is complete.
let res = assert.commandWorked(newPrimary.adminCommand({replSetGetStatus: 1}));
assert(
    res.electionCandidateMetrics,
    () => "Response should have an 'electionCandidateMetrics' field: " + tojson(res),
);
assert(
    res.electionCandidateMetrics.newTermStartDate,
    () =>
        "Response should have an 'electionCandidateMetrics.newTermStartDate' field: " +
        tojson(res.electionCandidateMetrics),
);

// Check that the 'electionCandidateMetrics' section of the replSetGetStatus response does not have
// the 'wMajorityWriteAvailabilityDate' field before the new term oplog entry has been replicated.
assert(
    !res.electionCandidateMetrics.wMajorityWriteAvailabilityDate,
    () =>
        "Response should not have an 'electionCandidateMetrics.wMajorityWriteAvailabilityDate' field: " +
        tojson(res.electionCandidateMetrics),
);

restartReplSetReplication(rst);
rst.awaitLastOpCommitted();

// Check that the 'electionCandidateMetrics' section of the replSetGetStatus response has the
// 'wMajorityWriteAvailabilityDate' field once the new term oplog entry is in the committed
// snapshot.
res = assert.commandWorked(newPrimary.adminCommand({replSetGetStatus: 1}));
assert(
    res.electionCandidateMetrics,
    () => "Response should have an 'electionCandidateMetrics' field: " + tojson(res),
);
assert(
    res.electionCandidateMetrics.wMajorityWriteAvailabilityDate,
    () =>
        "Response should have an 'electionCandidateMetrics.wMajorityWriteAvailabilityDate' field: " +
        tojson(res.electionCandidateMetrics),
);

rst.stopSet();
