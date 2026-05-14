/**
 * Verifies that the "phase" field of the featureCompatibilityVersion document is only persisted
 * when featureFlagSymmetricFCV is enabled.
 *
 * When the flag is off, intermediate setFCV transitions must NOT write a "phase" field into
 * admin.system.version. Otherwise the field leaks the in-flight transition state to:
 *   - external observers querying admin.system.version on a primary or secondary,
 *   - lagging secondaries running an older binary that does not recognise the field
 *     (no oplog application error is raised today, but the field is silently absorbed and
 *      becomes visible to clients reading the doc).
 *
 * This test exercises the mixed-binary scenario:
 *   - primary on "latest" binary,
 *   - secondary on "last-lts" binary (does not know "phase"),
 *   - FCV held in an intermediate transitional state via a hang-before-finalize failpoint,
 *   - secondary's view of the FCV doc inspected before the transition completes.
 *
 * @tags: [
 *   requires_replication,
 *   multiversion_incompatible,
 * ]
 */

import "jstests/multiVersion/libs/multi_rs.js";
import "jstests/multiVersion/libs/verify_versions.js";

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kFcvNs = "admin.system.version";
const kFcvId = "featureCompatibilityVersion";

/**
 * Reads the FCV document from the supplied connection and returns the "phase" field, or undefined
 * if absent.
 */
function readPhase(conn) {
    const doc = conn.getDB("admin").system.version.findOne({_id: kFcvId});
    assert.neq(null, doc, `expected an FCV doc in ${kFcvNs}`);
    return doc.phase;
}

/**
 * Asserts that an oplog entry that targeted the FCV document does not contain a "phase" field
 * once we have proven SymmetricFCV is off. We use the oplog rather than the secondary's local
 * collection because some older binaries silently absorb unknown top-level fields on update.
 */
function assertNoPhaseInRecentFcvOplog(primary) {
    const oplog = primary.getDB("local").oplog.rs;
    const entries = oplog
        .find({ns: kFcvNs})
        .sort({$natural: -1})
        .limit(10)
        .toArray();
    for (const e of entries) {
        // Replacement updates land in `o`; delta updates land in `o.diff`.
        if (e.o && e.o.phase !== undefined) {
            assert(false, `oplog entry on ${kFcvNs} unexpectedly carries phase: ${tojson(e)}`);
        }
        if (e.o && e.o.diff && e.o.diff.i && e.o.diff.i.phase !== undefined) {
            assert(false,
                `oplog entry on ${kFcvNs} unexpectedly inserts phase: ${tojson(e)}`);
        }
        if (e.o && e.o.diff && e.o.diff.u && e.o.diff.u.phase !== undefined) {
            assert(false,
                `oplog entry on ${kFcvNs} unexpectedly updates phase: ${tojson(e)}`);
        }
    }
}

const rst = new ReplSetTest({
    name: jsTestName(),
    nodes: [
        {binVersion: "latest"},
        {binVersion: "last-lts", rsConfig: {priority: 0, votes: 0}},
    ],
});

rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const primaryAdmin = primary.getDB("admin");

// Sanity check that we wound up with the expected topology. The helpers below come from
// jstests/multiVersion/libs/verify_versions.js and operate on Mongo connections directly.
assert.binVersion(primary, "latest");
assert.binVersion(secondary, "last-lts");

const symmetricEnabled = FeatureFlagUtil.isEnabled(primaryAdmin, "SymmetricFCV");
jsTestLog(`featureFlagSymmetricFCV enabled on primary: ${symmetricEnabled}`);

// Make sure the cluster is at latestFCV so the next setFCV step downgrades through the
// intermediate transitional state where `phase` would historically appear.
assert.commandWorked(
    primaryAdmin.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
);
rst.awaitReplication();
checkFCV(primaryAdmin, latestFCV);

// Park setFCV inside the transitional window: the doc has been written to the transitional FCV
// but the final-state write has not happened yet. This is the precise window during which
// `phase` is observable.
const hangFp = configureFailPoint(primary, "hangBeforeFinalizingFCV");

const setFcvAwait = startParallelShell(() => {
    assert.commandWorked(
        db.getSiblingDB("admin").runCommand({
            setFeatureCompatibilityVersion: lastLTSFCV,
            confirm: true,
        }),
    );
}, primary.port);

hangFp.wait();

try {
    // Force the secondary to apply everything that has been replicated so far before we read.
    rst.awaitReplication();

    const primaryPhase = readPhase(primary);
    const secondaryPhase = readPhase(secondary);

    jsTestLog(`Primary phase observed: ${tojson(primaryPhase)}`);
    jsTestLog(`Secondary phase observed: ${tojson(secondaryPhase)}`);

    if (symmetricEnabled) {
        // With the flag on, intermediate phase IS allowed to be persisted. The secondary may not
        // know the field but the latest binary on the primary is permitted to write it. We only
        // assert that the primary and secondary agree (replication faithfully delivered it).
        assert.eq(
            primaryPhase,
            secondaryPhase,
            "primary and secondary disagree on phase under symmetric flag",
        );
    } else {
        // With the flag off, no observer (primary OR secondary) is permitted to see a phase
        // field, because the writer must not have persisted one in the first place.
        assert.eq(
            undefined,
            primaryPhase,
            `primary leaked phase field with SymmetricFCV disabled: ${tojson(primaryPhase)}`,
        );
        assert.eq(
            undefined,
            secondaryPhase,
            `secondary leaked phase field with SymmetricFCV disabled: ${tojson(secondaryPhase)}`,
        );
        // Belt-and-braces: also verify the oplog does not carry the field, which is what
        // last-lts binaries would have to absorb.
        assertNoPhaseInRecentFcvOplog(primary);
    }

    // Final invariant for both flag states: the older-binary secondary must not have logged an
    // oplog-application error. If it did, replication would already be broken. Probing it via a
    // trivial write confirms the secondary is still applying.
    assert.commandWorked(primary.getDB("test").canary.insert({_id: "canary"}));
    rst.awaitReplication();
    assert.eq(
        1,
        secondary.getDB("test").canary.find({_id: "canary"}).itcount(),
        "secondary stopped applying oplog after intermediate-phase FCV write",
    );
} finally {
    hangFp.off();
    setFcvAwait();
}

rst.awaitReplication();
checkFCV(primaryAdmin, lastLTSFCV);

// And one more invariant: the terminal state must NEVER carry a phase field, regardless of the
// flag. This is the contract the existing code base relies on for the post-transition document.
const terminalPhasePrimary = readPhase(primary);
const terminalPhaseSecondary = readPhase(secondary);
assert.eq(undefined, terminalPhasePrimary, "terminal FCV doc on primary still has phase");
assert.eq(undefined, terminalPhaseSecondary, "terminal FCV doc on secondary still has phase");

rst.stopSet();
