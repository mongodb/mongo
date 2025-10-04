/**
 * Performs updates that will change a document's shard key across chunks while simultaneously
 * changing the FCV.
 *
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 *  uses_transactions,
 *  # Requires all nodes to be running the latest binary.
 *  multiversion_incompatible,
 *  # TODO (SERVER-104171) Re-enable this test once downgrade works with chunk migrations.
 *  __TEMPORARILY_DISABLED__,
 * ]
 */

import "jstests/libs/override_methods/retry_on_killed_session.js";

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {handleRandomSetFCVErrors} from "jstests/concurrency/fsm_workload_helpers/fcv/handle_setFCV_errors.js";
import {assertSetFCVSoon} from "jstests/concurrency/fsm_workload_helpers/query/assert_fcv_reset_soon.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/random_moveChunk/random_moveChunk_update_shard_key.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    // Sessions of open transactions can be killed and throw "Interrupted" if we run it concurrently
    // with a setFCV command, so we want to be able to catch those as acceptable killSession errors.
    $config.data.retryOnKilledSession = true;

    $config.states.setFCV = function (db, collName, connCache) {
        const fcvValues = [lastLTSFCV, lastContinuousFCV, latestFCV];
        const targetFCV = fcvValues[Random.randInt(3)];
        jsTestLog("Executing FCV state, setting to:" + targetFCV);
        try {
            assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));
        } catch (e) {
            if (handleRandomSetFCVErrors(e, targetFCV)) return;
            throw e;
        }
        jsTestLog("setFCV state finished");
    };

    // Only including states from the base workload that can trigger a WCOS error, since that is
    // currently a code path that uses internal transactions.
    $config.transitions = {
        init: {
            findAndModifyWithRetryableWriteAcrossChunks: 0.2,
            findAndModifyWithTransactionAcrossChunks: 0.2,
            updateWithRetryableWriteAcrossChunks: 0.2,
            updateWithTransactionAcrossChunks: 0.2,
            setFCV: 0.2,
        },
        findAndModifyWithRetryableWriteAcrossChunks: {
            findAndModifyWithRetryableWriteAcrossChunks: 0.2,
            findAndModifyWithTransactionAcrossChunks: 0.2,
            updateWithRetryableWriteAcrossChunks: 0.2,
            updateWithTransactionAcrossChunks: 0.2,
            verifyDocuments: 0.1,
            setFCV: 0.1,
        },
        findAndModifyWithTransactionAcrossChunks: {
            findAndModifyWithRetryableWriteAcrossChunks: 0.2,
            findAndModifyWithTransactionAcrossChunks: 0.2,
            updateWithRetryableWriteAcrossChunks: 0.2,
            updateWithTransactionAcrossChunks: 0.2,
            verifyDocuments: 0.1,
            setFCV: 0.1,
        },
        updateWithRetryableWriteAcrossChunks: {
            findAndModifyWithRetryableWriteAcrossChunks: 0.2,
            findAndModifyWithTransactionAcrossChunks: 0.2,
            updateWithRetryableWriteAcrossChunks: 0.2,
            updateWithTransactionAcrossChunks: 0.2,
            verifyDocuments: 0.1,
            setFCV: 0.1,
        },
        updateWithTransactionAcrossChunks: {
            findAndModifyWithRetryableWriteAcrossChunks: 0.2,
            findAndModifyWithTransactionAcrossChunks: 0.2,
            updateWithRetryableWriteAcrossChunks: 0.2,
            updateWithTransactionAcrossChunks: 0.2,
            verifyDocuments: 0.1,
            setFCV: 0.1,
        },
        verifyDocuments: {
            findAndModifyWithRetryableWriteAcrossChunks: 0.2,
            findAndModifyWithTransactionAcrossChunks: 0.2,
            updateWithRetryableWriteAcrossChunks: 0.2,
            updateWithTransactionAcrossChunks: 0.2,
            verifyDocuments: 0.1,
            setFCV: 0.1,
        },
        setFCV: {
            findAndModifyWithRetryableWriteAcrossChunks: 0.2,
            findAndModifyWithTransactionAcrossChunks: 0.2,
            updateWithRetryableWriteAcrossChunks: 0.2,
            updateWithTransactionAcrossChunks: 0.2,
            verifyDocuments: 0.1,
            setFCV: 0.1,
        },
    };

    $config.teardown = function (db, collName, cluster) {
        assertSetFCVSoon(db, latestFCV);
    };

    return $config;
});
