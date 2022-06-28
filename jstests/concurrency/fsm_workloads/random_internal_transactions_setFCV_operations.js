'use strict';

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
 * ]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/random_moveChunk_update_shard_key.js');
// Transactions that run concurrently with a setFCV may get interrupted due to setFCV issuing for a
// killSession any open sessions during an FCV change. We want to have to retryability support for
// such scenarios.
load('jstests/libs/override_methods/retry_on_killed_session.js');

var $config = extendWorkload($config, function($config, $super) {
    // Sessions of open transactions can be killed and throw "Interrupted" if we run it concurrently
    // with a setFCV command, so we want to be able to catch those as acceptable killSession errors.
    $config.data.retryOnKilledSession = true;

    $config.states.setFCV = function(db, collName, connCache) {
        const fcvValues = [lastLTSFCV, lastContinuousFCV, latestFCV];
        const targetFCV = fcvValues[Random.randInt(3)];
        jsTestLog('Executing FCV state, setting to:' + targetFCV);
        try {
            assertAlways.commandWorked(
                db.adminCommand({setFeatureCompatibilityVersion: targetFCV}));
        } catch (e) {
            if (e.code === 5147403) {
                // Invalid fcv transition (e.g lastContinuous -> lastLTS)
                jsTestLog('setFCV: Invalid transition');
                return;
            }
            throw e;
        }
        jsTestLog('setFCV state finished');
    };

    // Only including states from the base workload that can trigger a WCOS error, since that is
    // currently a code path that uses internal transactions.
    $config.transitions = {
        init: {
            findAndModifyWithRetryableWriteAcrossChunks: 0.2,
            findAndModifyWithTransactionAcrossChunks: 0.2,
            updateWithRetryableWriteAcrossChunks: 0.2,
            updateWithTransactionAcrossChunks: 0.2,
            setFCV: 0.2
        },
        findAndModifyWithRetryableWriteAcrossChunks: {
            findAndModifyWithRetryableWriteAcrossChunks: 0.2,
            findAndModifyWithTransactionAcrossChunks: 0.2,
            updateWithRetryableWriteAcrossChunks: 0.2,
            updateWithTransactionAcrossChunks: 0.2,
            verifyDocuments: 0.1,
            setFCV: 0.1
        },
        findAndModifyWithTransactionAcrossChunks: {
            findAndModifyWithRetryableWriteAcrossChunks: 0.2,
            findAndModifyWithTransactionAcrossChunks: 0.2,
            updateWithRetryableWriteAcrossChunks: 0.2,
            updateWithTransactionAcrossChunks: 0.2,
            verifyDocuments: 0.1,
            setFCV: 0.1
        },
        updateWithRetryableWriteAcrossChunks: {
            findAndModifyWithRetryableWriteAcrossChunks: 0.2,
            findAndModifyWithTransactionAcrossChunks: 0.2,
            updateWithRetryableWriteAcrossChunks: 0.2,
            updateWithTransactionAcrossChunks: 0.2,
            verifyDocuments: 0.1,
            setFCV: 0.1
        },
        updateWithTransactionAcrossChunks: {
            findAndModifyWithRetryableWriteAcrossChunks: 0.2,
            findAndModifyWithTransactionAcrossChunks: 0.2,
            updateWithRetryableWriteAcrossChunks: 0.2,
            updateWithTransactionAcrossChunks: 0.2,
            verifyDocuments: 0.1,
            setFCV: 0.1
        },
        verifyDocuments: {
            findAndModifyWithRetryableWriteAcrossChunks: 0.2,
            findAndModifyWithTransactionAcrossChunks: 0.2,
            updateWithRetryableWriteAcrossChunks: 0.2,
            updateWithTransactionAcrossChunks: 0.2,
            verifyDocuments: 0.1,
            setFCV: 0.1
        },
        setFCV: {
            findAndModifyWithRetryableWriteAcrossChunks: 0.2,
            findAndModifyWithTransactionAcrossChunks: 0.2,
            updateWithRetryableWriteAcrossChunks: 0.2,
            updateWithTransactionAcrossChunks: 0.2,
            verifyDocuments: 0.1,
            setFCV: 0.1
        },

    };

    $config.teardown = function(db, collName, cluster) {
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    };

    return $config;
});
