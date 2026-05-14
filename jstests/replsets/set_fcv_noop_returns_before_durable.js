/**
 * SERVER-120978: a no-op setFeatureCompatibilityVersion call (requested ==
 * actual) must not return ok:1 before the FCV state it reports is majority
 * durable. The bug path waits on a stale ReplClientInfo::lastOp; the fix
 * (setLastOpToSystemLastOpTime) pins the system last applied opTime so the
 * majority-wait dominates every prior FCV write.
 *
 * Repro shape:
 *   1. Initiate a 3-node replset at latestFCV.
 *   2. Block secondaries from replicating via the stopReplProducer failpoint.
 *   3. Issue a real downgrade setFCV on the primary in a parallel shell;
 *      the FCV-doc write lands on the primary but cannot reach majority
 *      because secondaries are paused.
 *   4. Step down the primary and elect a new one (the old primary's pending
 *      write has not majority-committed, so it can roll back).
 *   5. On the new primary, issue a NO-OP setFCV at latestFCV (matches the
 *      pre-downgrade durable state). Under the bug, this returns ok:1 while
 *      the prior FCV mutation is still in flight.
 *   6. Assert that after the no-op returns ok:1, the FCV observed on a
 *      majority-read is consistent with what the no-op effectively
 *      confirmed.
 *
 * @tags: [
 *     multiversion_incompatible,
 *     requires_majority_read_concern,
 *     requires_replication,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    name: jsTestName(),
    nodes: 3,
    nodeOptions: {setParameter: {logComponentVerbosity: tojson({command: 2, replication: 2})}},
});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

let primary = rst.getPrimary();
let secondaries = rst.getSecondaries();
const adminPrimary = primary.getDB("admin");

jsTestLog("Confirm starting FCV is latestFCV.");
checkFCV(adminPrimary, latestFCV);

jsTestLog("Pause replication on both secondaries so the downgrade write cannot " +
          "majority-commit.");
const fpSecondary0 = configureFailPoint(secondaries[0], "stopReplProducer");
const fpSecondary1 = configureFailPoint(secondaries[1], "stopReplProducer");

jsTestLog("Block the primary right before it returns from the writing-path FCV " +
          "downgrade, so the parallel shell stays parked while we step it down.");
const fpHangBeforeReturn = configureFailPoint(primary, "hangBeforeUpdatingFcvDoc");

jsTestLog("Kick off downgrade setFCV on primary in a parallel shell.");
const parallelDowngrade = startParallelShell(`
    const res = db.adminCommand({
        setFeatureCompatibilityVersion: "${lastLTSFCV}",
        confirm: true,
        writeConcern: {w: "majority", wtimeout: 5000},
    });
    // We expect this to either fail due to the step-down we are about to
    // force, or to wtimeout because secondaries are paused. Either way the
    // operation must NOT have successfully majority-committed the FCV
    // change.
    assert(!res.ok || res.code !== 0 ||
           (res.writeConcernError !== undefined),
           "downgrade should not silently majority-commit while secondaries " +
           "are paused; got: " + tojson(res));
`, primary.port);

fpHangBeforeReturn.wait();

jsTestLog("Force a step-down so the pending FCV write is now eligible to roll " +
          "back if the new primary diverges.");
assert.commandWorked(
    primary.adminCommand({replSetStepDown: 60, force: true}));

fpHangBeforeReturn.off();

jsTestLog("Resume replication so a new primary can be elected.");
fpSecondary0.off();
fpSecondary1.off();

rst.awaitSecondaryNodes(null, [primary]);
rst.awaitNodesAgreeOnPrimary();

const newPrimary = rst.getPrimary();
assert.neq(newPrimary.host, primary.host, "expected a new primary after step-down");
const adminNewPrimary = newPrimary.getDB("admin");

jsTestLog("Issue a no-op setFCV at latestFCV on the new primary. Under the " +
          "buggy code path this returns immediately on the client's stored " +
          "opTime (stale) instead of pinning the system last applied opTime.");
const noopRes = assert.commandWorked(adminNewPrimary.runCommand({
    setFeatureCompatibilityVersion: latestFCV,
    confirm: true,
    writeConcern: {w: "majority", wtimeout: ReplSetTest.kDefaultTimeoutMS},
}));
assert(!noopRes.writeConcernError,
       "no-op setFCV must not return a writeConcernError on the fixed path; got: " +
       tojson(noopRes));

jsTestLog("After the no-op returns ok:1, a majority-read FCV on every node " +
          "must reflect the confirmed state.");
rst.awaitReplication();

for (const node of rst.nodes) {
    const adminN = node.getDB("admin");
    // A majority read of the FCV document must show latestFCV, matching the
    // version the no-op setFCV just confirmed durable.
    const fcvDoc = assert.commandWorked(adminN.runCommand({
        find: "system.version",
        filter: {_id: "featureCompatibilityVersion"},
        readConcern: {level: "majority"},
    }));
    assert.eq(fcvDoc.cursor.firstBatch.length, 1,
              "FCV doc missing on " + node.host + ": " + tojson(fcvDoc));
    const observed = fcvDoc.cursor.firstBatch[0].version;
    assert.eq(observed, latestFCV,
              "SERVER-120978: node " + node.host + " observed FCV " + observed +
              " after no-op setFCV reported ok:1 at " + latestFCV +
              "; the no-op majority-wait must have pinned the system last " +
              "applied opTime, not the client's stored opTime.");
}

parallelDowngrade();

jsTestLog("Final consistency: every node agrees on FCV via majority read.");
checkFCV(adminNewPrimary, latestFCV);
for (const sec of rst.getSecondaries()) {
    checkFCV(sec.getDB("admin"), latestFCV);
}

rst.stopSet();
