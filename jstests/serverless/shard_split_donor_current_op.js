/**
 * Tests currentOp command during a shard split.
 *
 * Shard splits are not expected to be run on servers with ephemeralForTest.
 *
 * @tags: [
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   featureFlagShardSplit,
 *   requires_fcv_52
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/serverless/libs/basic_serverless_test.js");

const kTenantIds = ["testTenantId"];

function checkStandardFieldsOK(ops, {migrationId, reachedDecision, tenantIds}) {
    assert.eq(ops.length, 1);
    const [op] = ops;
    assert.eq(bsonWoCompare(op.instanceID, migrationId), 0);
    assert.eq(op.reachedDecision, reachedDecision);

    if (tenantIds) {
        assert.eq(bsonWoCompare(op.tenantIds, tenantIds), 0);
    }
}

(() => {
    jsTestLog("Testing currentOp output for split before blocking state");
    const test = new BasicServerlessTest(
        {recipientTagName: "recipientTag", recipientSetName: "recipientSet"});
    test.addRecipientNodes();

    const operation = test.createSplitOperation(kTenantIds);

    const donorPrimary = test.donor.getPrimary();
    let fp = configureFailPoint(donorPrimary, "pauseShardSplitBeforeBlockingState");
    const splitThread = operation.commitAsync();

    fp.wait();

    const res = assert.commandWorked(
        donorPrimary.adminCommand({currentOp: true, desc: "shard split operation"}));
    print(`CURR_OP: ${tojson(res)}`);

    checkStandardFieldsOK(
        res.inprog,
        {migrationId: operation.migrationId, reachedDecision: false, tenantIds: kTenantIds});
    assert(!res.inprog[0].blockOpTime);

    fp.off();

    splitThread.join();
    assert.commandWorked(splitThread.returnData());

    test.removeAndStopRecipientNodes();
    test.stop();
})();

(() => {
    jsTestLog("Testing currentOp output for split in blocking state");
    const test = new BasicServerlessTest(
        {recipientTagName: "recipientTag", recipientSetName: "recipientSet"});
    test.addRecipientNodes();

    const operation = test.createSplitOperation(kTenantIds);

    const donorPrimary = test.donor.getPrimary();

    let fp = configureFailPoint(donorPrimary, "pauseShardSplitAfterBlocking");

    const splitThread = operation.commitAsync();

    fp.wait();

    const res = assert.commandWorked(
        donorPrimary.adminCommand({currentOp: true, desc: "shard split operation"}));

    checkStandardFieldsOK(
        res.inprog,
        {migrationId: operation.migrationId, reachedDecision: false, tenantIds: kTenantIds});
    assert(res.inprog[0].blockOpTime.ts instanceof Timestamp);

    fp.off();

    splitThread.join();
    assert.commandWorked(splitThread.returnData());

    test.removeAndStopRecipientNodes();
    test.stop();
})();

(() => {
    jsTestLog("Testing currentOp output for aborted split");
    const test = new BasicServerlessTest(
        {recipientTagName: "recipientTag", recipientSetName: "recipientSet"});
    test.addRecipientNodes();

    const operation = test.createSplitOperation(kTenantIds);
    assert.commandWorked(operation.abort());

    const res = assert.commandWorked(
        test.donor.getPrimary().adminCommand({currentOp: true, desc: "shard split operation"}));

    checkStandardFieldsOK(
        res.inprog, {migrationId: operation.migrationId, reachedDecision: true, tenantIds: null});
    assert(res.inprog[0].commitOrAbortOpTime.ts instanceof Timestamp);
    assert(res.inprog[0].commitOrAbortOpTime.t instanceof NumberLong);
    assert.eq(typeof res.inprog[0].abortReason.code, "number");
    assert.eq(typeof res.inprog[0].abortReason.codeName, "string");
    assert.eq(typeof res.inprog[0].abortReason.errmsg, "string");
    assert(res.inprog[0].expireAt instanceof Date);

    test.stop();
})();

// Check currentOp while in committed state before and after a split has completed.
(() => {
    jsTestLog("Testing currentOp output for committed split");
    const test = new BasicServerlessTest(
        {recipientTagName: "recipientTag", recipientSetName: "recipientSet"});
    test.addRecipientNodes();

    const donorPrimary = test.donor.getPrimary();

    const operation = test.createSplitOperation(kTenantIds);
    assert.commandWorked(operation.commit());

    let res = donorPrimary.adminCommand({currentOp: true, desc: "shard split operation"});

    checkStandardFieldsOK(
        res.inprog,
        {migrationId: operation.migrationId, reachedDecision: true, tenantIds: kTenantIds});
    assert(res.inprog[0].blockOpTime.ts instanceof Timestamp);
    assert(res.inprog[0].commitOrAbortOpTime.ts instanceof Timestamp);
    assert(res.inprog[0].commitOrAbortOpTime.t instanceof NumberLong);
    assert(!res.inprog[0].expireAt);
    assert.eq(res.inprog[0].recipientSetName, operation.recipientSetName);
    assert.eq(res.inprog[0].recipientTagName, operation.recipientTagName);
    assert(res.inprog[0].recipientConnectionString);

    jsTestLog("Testing currentOp output for a committed split after forgetShardSplit");

    test.removeAndStopRecipientNodes();
    operation.forget();

    res = test.donor.getPrimary().adminCommand({currentOp: true, desc: "shard split operation"});

    jsTestLog("Checking");
    checkStandardFieldsOK(
        res.inprog,
        {migrationId: operation.migrationId, reachedDecision: true, tenantIds: kTenantIds});
    assert(res.inprog[0].blockOpTime.ts instanceof Timestamp);
    assert(res.inprog[0].commitOrAbortOpTime.ts instanceof Timestamp);
    assert(res.inprog[0].commitOrAbortOpTime.t instanceof NumberLong);
    assert(res.inprog[0].expireAt instanceof Date);

    test.stop();
})();
})();
