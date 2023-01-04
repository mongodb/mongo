/**
 * Starts a shard split operation that aborts, either due to the
 * abortShardSplitBeforeLeavingBlockingState failpoint or due to receiving abortShardSplit,
 * and then issues a forgetShardSplit command. Finally, starts a second shard split operation with
 * the same tenantIds as the aborted shard split, and expects this second one to go through.
 *
 * @tags: [requires_fcv_62, serverless]
 */

import {assertMigrationState, ShardSplitTest} from "jstests/serverless/libs/shard_split_test.js";

load("jstests/libs/fail_point_util.js");

TestData.skipCheckDBHashes = true;
const test = new ShardSplitTest({quickGarbageCollection: true});

(() => {
    const tenantIds = [ObjectId(), ObjectId()];
    test.addRecipientNodes();

    const donorPrimary = test.donor.getPrimary();
    const abortFp = configureFailPoint(donorPrimary, "abortShardSplitBeforeLeavingBlockingState");

    const operation = test.createSplitOperation(tenantIds);

    // Start a shard split with the "abortShardSplitBeforeLeavingBlockingState" failPoint
    // enabled. The split will abort as a result, and a status of "kAborted" should be returned.
    jsTestLog(
        `Starting a shard split that is expected to abort due to setting
            abortShardSplitBeforeLeavingBlockingState failpoint. migrationId:
            ${operation.migrationId} , tenantIds: ${tojson(tenantIds)}`);

    operation.commit();
    abortFp.off();

    assertMigrationState(donorPrimary, operation.migrationId, "aborted");

    jsTestLog(`Forgetting aborted shard split with migrationId: ${operation.migrationId}`);
    operation.forget();
    test.cleanupSuccesfulAborted(operation.migrationId, tenantIds);

    // Try running a new shard split with the same tenantId. It should succeed, since the previous
    // shard split with the same tenantId was aborted.
    test.addRecipientNodes();
    const operation2 = test.createSplitOperation(tenantIds);
    jsTestLog(
        `Attempting to run a shard split with the same tenantIds. New migrationId:
            ${operation2.migrationId}, tenantIds: ${tojson(tenantIds)}`);

    operation2.commit();
    assertMigrationState(donorPrimary, operation2.migrationId, "committed");

    operation2.forget();
    test.cleanupSuccesfulCommitted(operation2.migrationId, tenantIds);
})();

(() => {
    const tenantIds = [ObjectId(), ObjectId()];

    test.addRecipientNodes();

    const donorPrimary = test.donor.getPrimary();
    let fp = configureFailPoint(donorPrimary, "pauseShardSplitAfterBlocking");

    const operation = test.createSplitOperation(tenantIds);
    jsTestLog(
        `Starting a shard split that is expected to abort in blocking state due to receiving
        abortShardSplit. migrationId: ${operation.migrationId}, tenantIds: ${tojson(tenantIds)}`);
    const commitAsyncRes = operation.commitAsync();

    fp.wait();

    assertMigrationState(donorPrimary, operation.migrationId, "blocking");

    operation.abort();

    fp.off();

    commitAsyncRes.join();

    assertMigrationState(donorPrimary, operation.migrationId, "aborted");

    jsTestLog(`Forgetting aborted shard split with migrationId: ${operation.migrationId}`);
    operation.forget();
    test.cleanupSuccesfulAborted(operation.migrationId, tenantIds);

    // Try running a new shard split with the same tenantId. It should succeed, since the previous
    // shard split with the same tenantId was aborted.
    test.addRecipientNodes();
    const operation2 = test.createSplitOperation(tenantIds);
    jsTestLog(
        `Attempting to run a new shard split with the same tenantIds. New migrationId:
        ${operation2.migrationId}, tenantIds: ${tojson(tenantIds)}`);
    operation2.commit();

    assertMigrationState(donorPrimary, operation2.migrationId, "committed");
    operation2.forget();
    test.cleanupSuccesfulCommitted(operation2.migrationId, tenantIds);
})();

test.stop();
