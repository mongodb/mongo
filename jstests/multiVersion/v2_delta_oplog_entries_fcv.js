/*
 * This file tests the usage of $v: 2 oplog entries along with its associated server parameter and
 * the FCV flag.
 */
(function() {
"use strict";

load("jstests/libs/curop_helpers.js");  // For waitForCurOpByFailPoint().

const kLatest = "latest";
const kLastStable = "4.4";
const kCollName = "v2_delta_oplog_entries_fcv";

// When attempting to apply a $v: 2 oplog entry on a node with bin version latest and FCV set to
// 4.4, this code is thrown.
const kErrorCodeThrownWhenApplyingV2OplogEntriesWithFCV44 = 4773100;

const kGiantStr = "x".repeat(100);

/**
 * Given a two-node ReplSetTest, sets the feature flag which enables V2 oplog entries on the
 * primary and secondary.
 */
function enableV2OplogEntries(rst) {
    const cmd = {setParameter: 1, internalQueryEnableLoggingV2OplogEntries: true};
    assert.commandWorked(rst.getPrimary().adminCommand(cmd));
    assert.commandWorked(rst.getSecondary().adminCommand(cmd));
}

/**
 * Helper function which runs an update on the test document that is eligible to be logged as a
 * $v: 2 delta style oplog entry.
 */
const kTestDocId = 0;
function runV2EligibleUpdate(coll) {
    assert.commandWorked(coll.update({_id: kTestDocId}, [{$set: {a: {$add: ["$a", 1]}}}]));
}

/**
 * Returns the current value of the test document.
 */
function getTestDoc(coll) {
    const arr = coll.find({_id: kTestDocId}).toArray();
    assert.eq(arr.length, 1);
    return arr[0];
}

/**
 * Helper function which will find the latest oplog entry for the test document.
 */
function getLatestOplogEntryForDoc(primaryDB) {
    const oplogRes = primaryDB.getSiblingDB("local")["oplog.rs"]
                         .find({"o2._id": kTestDocId})
                         .hint({$natural: -1})
                         .limit(1)
                         .toArray();
    assert.eq(oplogRes.length, 1);
    return oplogRes[0];
}

/**
 * This function tests behavior around applying $v: 2 oplog entries. If 'errorCode' is null
 * then the function checks that $v: 2 entries can be applied. Otherwise, it checks that
 * attempting to apply a $v: 2 entry via applyOps results in the given code.
 */
function checkApplyOpsOfV2Entries(coll, errorCode) {
    let testDoc = getTestDoc(coll);

    // Check that applying a $v:2 entry directly using applyOps works as expected.
    const applyOpsRes = coll.getDB().adminCommand({
        applyOps: [{
            "op": "u",
            "ns": coll.getFullName(),
            "o2": {_id: kTestDocId},
            "o": {$v: NumberInt(2), diff: {u: {a: 0}}}
        }]
    });

    if (errorCode) {
        assert.commandFailedWithCode(applyOpsRes, errorCode);
    } else {
        assert.commandWorked(applyOpsRes);
        // Ensure that the diff was applied correctly and that 'a' now is equal to 0.
        testDoc.a = 0;
        assert.eq(getTestDoc(coll), testDoc);
    }
}

/**
 * Helper function which runs an update that would be eligible to use a $v: 2 oplog entry, and
 * then checks that the most recent oplog entry for the updated document is *not* $v: 2.
 */
function runUpdateAndCheckV2EntriesNotLogged(coll) {
    let testDoc = getTestDoc(coll);
    // Run an update which would be eligible to use a $v: 2 entry.
    runV2EligibleUpdate(coll);

    // Check that the oplog entry we create is not $v: 2.
    const oplogEntry = getLatestOplogEntryForDoc(coll.getDB());
    assert.neq(oplogEntry.o.$v, 2, oplogEntry);
    assert(!oplogEntry.o.hasOwnProperty("diff"));

    // Check that the update was applied, which incremented 'a'.
    testDoc.a += 1;
    assert.eq(getTestDoc(coll), testDoc);
}

/**
 * Similar to runUpdateAndCheckV2EntriesNotLogged() but checks that a $v: 2 entry _is_ logged.
 */
function runUpdateAndCheckV2EntriesLogged(coll) {
    let testDoc = getTestDoc(coll);

    // Run an update which would be eligible to use a $v: 2 entry.
    runV2EligibleUpdate(coll);

    // Check that the oplog entry we create is $v: 2.
    const oplogEntry = getLatestOplogEntryForDoc(coll.getDB());
    assert.eq(oplogEntry.o.$v, 2, oplogEntry);
    assert.eq(typeof (oplogEntry.o.diff), "object");

    // Check that the update was applied, which incremented 'a'.
    testDoc.a += 1;
    assert.eq(getTestDoc(coll), testDoc);
}

const rst = new ReplSetTest({nodes: 2, nodeOpts: {noCleanData: true}});

// Start a last stable replica set and ensure that $v: 2 oplog entries are not logged.
(function runLastStable() {
    rst.startSet({binVersion: kLastStable});
    // Initiate with high election timeout to prevent unplanned elections from happening.
    rst.initiateWithHighElectionTimeout();

    const primaryDB = rst.getPrimary().getDB("test");
    const coll = primaryDB[kCollName];

    // Insert the test document.
    assert.commandWorked(coll.insert({_id: kTestDocId, a: 1, padding: kGiantStr}));

    // We expect the primary not to log $v:2 oplog entries or to allow application of
    // them via applyOps.
    runUpdateAndCheckV2EntriesNotLogged(coll);

    // The error code used by 4.4 in this scenario is different from the one used in 4.7+.
    const k44ApplyOpsUnknownUpdateVersionErrorCode = 40682;
    checkApplyOpsOfV2Entries(coll, k44ApplyOpsUnknownUpdateVersionErrorCode);

    rst.stopSet(
        null,  // signal
        true   // for restart
    );
})();

// Start a latest replica set using the same data files. The set should start in FCV 4.4 by
// default.
(function runLatest() {
    const nodes = rst.startSet({restart: true, binVersion: kLatest});
    rst.awaitNodesAgreeOnAppliedOpTime();
    // Step up node 0. Since we started with a high election timeout this would otherwise
    // take a while.
    assert.commandWorked(nodes[0].adminCommand({replSetStepUp: 1}));

    const primaryAdminDB = rst.getPrimary().getDB("admin");
    checkFCV(primaryAdminDB, kLastStable);
    const coll = rst.getPrimary().getDB("test")[kCollName];
    const oplog = primaryAdminDB.getSiblingDB("local")["oplog.rs"];

    // We should not log, or allow application of $v: 2 entries while in FCV 4.4.
    runUpdateAndCheckV2EntriesNotLogged(coll);
    checkApplyOpsOfV2Entries(coll, kErrorCodeThrownWhenApplyingV2OplogEntriesWithFCV44);

    // Now set the feature flag which allows $v: 2 oplog entries. Even with the flag enabled,
    // $v:2 oplog entries should not be used because the FCV is still 4.4.
    enableV2OplogEntries(rst);
    runUpdateAndCheckV2EntriesNotLogged(coll);
    checkApplyOpsOfV2Entries(coll, kErrorCodeThrownWhenApplyingV2OplogEntriesWithFCV44);

    // Upgrade to the new FCV. Now $v: 2 oplog entries will be logged (and applied).
    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(primaryAdminDB, latestFCV);
    runUpdateAndCheckV2EntriesLogged(coll);
    checkApplyOpsOfV2Entries(coll);

    rst.awaitReplication();

    // Disable the flag which allows *logging* of $v: 2 entries, but only on the secondary. We
    // check that nothing goes wrong when the primary logs a $v: 2 oplog entry. The secondary
    // should still be able to apply it.
    assert.commandWorked(rst.getSecondary().adminCommand(
        {setParameter: 1, internalQueryEnableLoggingV2OplogEntries: false}));
    runUpdateAndCheckV2EntriesLogged(coll);
    checkApplyOpsOfV2Entries(coll);
    rst.awaitReplication();

    // Check that if we disable the flag which allows *logging* of $v: 2 entries on the primary
    // but enable it on the secondary, a $v: 2 entry is not used.
    assert.commandWorked(rst.getSecondary().adminCommand(
        {setParameter: 1, internalQueryEnableLoggingV2OplogEntries: true}));
    assert.commandWorked(rst.getPrimary().adminCommand(
        {setParameter: 1, internalQueryEnableLoggingV2OplogEntries: false}));
    runUpdateAndCheckV2EntriesNotLogged(coll);

    // Although we don't expect a $v: 2 entry to be logged, application should still be allowed.
    checkApplyOpsOfV2Entries(coll);
    rst.awaitReplication();

    // Re-enable the parameter that controls logging of $v: 2 oplog entries on both the primary
    // and second. Then, downgrade the FCV back to 4.4. $v: 2 oplog entries should not be used.
    enableV2OplogEntries(rst);

    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    checkFCV(primaryAdminDB, lastLTSFCV);
    runUpdateAndCheckV2EntriesNotLogged(coll);
    checkApplyOpsOfV2Entries(coll, kErrorCodeThrownWhenApplyingV2OplogEntriesWithFCV44);
    rst.awaitReplication();

    /**
     * Function intended to be run by a parallel shell that will downgrade the FCV.
     */
    function downgradeFCVParallelShellFn() {
        assert.commandWorked(
            db.getSiblingDB("admin").runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    }

    /**
     * Waits until an oplog entry which puts the set into a "downgrading" state is logged at time
     * greater than 'afterTs'.
     */
    function waitUntilDowngradingOplogEntryLogged(afterTs) {
        assert.soon(function() {
            // Wait until a (recent) oplog entry changing the FCV document appears.
            return oplog.find({ts: {$gt: afterTs}, "o.targetVersion": "4.4"}).itcount() > 0;
        });
    }

    // We will now test the situation where the FCV is downgraded _while_ a $v:2 eligible pipeline
    // update is running. In particular, we want to check that if a $v: 2 entry is logged
    // while the RS is in the "downgrading" state (meaning the FCV target version is set to 4.4)
    // nothing bad happens.
    (function testFCVDowngradeWhileV2UpdateRuns() {
        // First set the FCV back to latest.
        assert.commandWorked(
            primaryAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
        checkFCV(primaryAdminDB, latestFCV);

        // Determine the last timestamp in the oplog. We'll use this later when checking that the
        // sequence of oplog entries recorded is expected.
        const lastTs = oplog.find().hint({$natural: -1}).limit(1).toArray()[0].ts;

        // Insert another test document.
        assert.commandWorked(coll.insert({_id: 1, padding: kGiantStr}));

        // First we are going to run a $v:2 eligible update and have it hang after it checks the
        // FCV. It will read a value of 4.7+, and based on that, decide to log a $v: 2 oplog
        // entry.

        const kPipelineFCVCheckFPName = "hangAfterPipelineUpdateFCVCheck";
        assert.commandWorked(primaryAdminDB.runCommand(
            {configureFailPoint: kPipelineFCVCheckFPName, mode: "alwaysOn"}));
        function runUpdateParallelShellFn() {
            assert.commandWorked(
                db.v2_delta_oplog_entries_fcv.update({_id: 1}, [{$set: {"x": 0}}]));
        }
        const joinUpdateShell = startParallelShell(runUpdateParallelShellFn, rst.getPrimary().port);

        // Wait until the update is hanging on the fail point. This means that the update has read
        // the FCV value and "decided" to take the $v: 2 code path.
        waitForCurOpByFailPointNoNS(coll.getDB(), kPipelineFCVCheckFPName);

        // Now start another parallel shell to downgrade the FCV.
        const joinDowngradeFCV =
            startParallelShell(downgradeFCVParallelShellFn, rst.getPrimary().port);

        // This downgrade of the FCV will record an oplog entry which indicates that the set is in a
        // "downgrading" state. After that, the primary will block attempting to acquire a MODE_S
        // lock, which conflicts with the update's MODE_IX lock.
        waitUntilDowngradingOplogEntryLogged(lastTs);

        // Unblock the update command currently running in a parallel shell. It will log a $v: 2
        // update oplog entry.
        assert.commandWorked(
            primaryAdminDB.runCommand({configureFailPoint: kPipelineFCVCheckFPName, mode: "off"}));
        joinUpdateShell();

        // Now that the update has run, the FCV change can complete.
        joinDowngradeFCV();

        // Check that the sequence of oplog entries is right. We expect to see the following
        // sequence, in ascending order by timestamp:
        // 1) Set target FCV to 4.4
        // 2) $v:2 update
        // 3) Set FCV to 4.4 (removing the 'targetVersion')
        // There may be other operations which happen in between these three, such as noop writes
        // and so on, so we find the timestamps for (1), (2) and (3) and check that they are in the
        // correct order.

        // Find the ts of (1).
        const setTargetFCVTimestamp =
            oplog.find({ts: {$gt: lastTs}, "o.targetVersion": "4.4"}).limit(1).toArray()[0].ts;

        // Find the ts of (2).
        const v2UpdateTimestamp =
            oplog.find({ts: {$gt: lastTs}, op: "u", "o.$v": 2}).limit(1).toArray()[0].ts;

        // Find the ts of (3).
        const setFCVFinalTimestamp =
            oplog
                .find(
                    {ts: {$gt: setTargetFCVTimestamp}, "o.targetVersion": null, "o.version": "4.4"})
                .limit(1)
                .toArray()[0]
                .ts;

        function dumpOplog() {
            oplog.find().hint({$natural: -1}).limit(10).toArray();
        }
        assert.lt(setTargetFCVTimestamp, v2UpdateTimestamp, dumpOplog);
        assert.lt(v2UpdateTimestamp, setFCVFinalTimestamp, dumpOplog);

        rst.awaitReplication();
        // Done. The sequence of oplog entries was expected, and the secondaries had no issues
        // applying the $v: 2 update while in the downgrading state.
    })();

    // Now we check that manual application of $v: 2 entries via applyOps while in the downgrading
    // state is banned.
    (function checkCannotUseV2EntryWithApplyOpsWhileDowngrading() {
        // First set the FCV back to latest.
        assert.commandWorked(
            primaryAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
        checkFCV(primaryAdminDB, latestFCV);

        // Determine the last timestamp in the oplog.
        const lastTs = oplog.find().sort({ts: -1}).limit(1).toArray()[0].ts;

        const kHangWhileDowngradingFP = "hangWhileDowngrading";
        primaryAdminDB.runCommand({configureFailPoint: kHangWhileDowngradingFP, mode: "alwaysOn"});

        // Begin a downgrade. It will hang while in the "downgrading" state.
        const joinDowngradeFCV =
            startParallelShell(downgradeFCVParallelShellFn, rst.getPrimary().port);

        // Wait until the node enters a "downgrading" state.
        waitUntilDowngradingOplogEntryLogged(lastTs);

        // The primary is now in a "downgrading" state. Attempts to log $v:2 oplog entries with
        // apply ops should fail.
        checkApplyOpsOfV2Entries(coll, kErrorCodeThrownWhenApplyingV2OplogEntriesWithFCV44);

        // Disable the fail point and join with the parallel shell.
        primaryAdminDB.runCommand({configureFailPoint: kHangWhileDowngradingFP, mode: "off"});
        joinDowngradeFCV();
    })();

    // Finally, as a last sanity test, we add a new node to the replica set. This should not
    // cause any issues.
    const newSecondary = rst.add({binVersion: kLatest});
    rst.reInitiate();
    rst.awaitReplication();

    rst.stopSet();
})();
})();
