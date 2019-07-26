'use strict';

/**
 * This test checks high level invariants of various transaction related metrics reported in
 * serverStatus and currentOp.
 *
 * @tags: [uses_transactions, uses_prepare_transaction, assumes_snapshot_transactions]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/multi_statement_transaction_atomicity_isolation.js');
load('jstests/core/txns/libs/prepare_helpers.js');
// for $config

var $config = extendWorkload($config, function($config, $super) {
    /**
     * Returns all elements in the given array that evaluate to false for the given predicate
     * function 'predFn'.
     */
    function filterFalse(arr, predFn) {
        return arr.filter(x => !predFn(x));
    }

    /**
     * serverStatus invariant: currentActive + currentInactive = currentOpen
     */
    function activePlusInactiveEqualsOpen(serverStatusTxnStats) {
        // Stats are returned in NumberLong type. Convert to Number type so we are sure comparison
        // works as expected.
        let active = Number(serverStatusTxnStats["currentActive"]);
        let inactive = Number(serverStatusTxnStats["currentInactive"]);
        let open = Number(serverStatusTxnStats["currentOpen"]);
        return (active + inactive) === open;
    }

    /**
     * serverStatus invariant: totalCommitted + totalAborted + currentOpen = totalStarted
     */
    function committedPlusAbortedPlusOpenEqualsStarted(serverStatusTxnStats) {
        let committed = Number(serverStatusTxnStats["totalCommitted"]);
        let aborted = Number(serverStatusTxnStats["totalAborted"]);
        let open = Number(serverStatusTxnStats["currentOpen"]);
        let started = Number(serverStatusTxnStats["totalStarted"]);
        return (committed + aborted + open) === started;
    }

    /**
     * serverStatus invariant: all counts are non-negative
     */
    function allCountsNonNegative(serverStatusTxnStats) {
        let active = Number(serverStatusTxnStats["currentActive"]);
        let inactive = Number(serverStatusTxnStats["currentInactive"]);
        let committed = Number(serverStatusTxnStats["totalCommitted"]);
        let aborted = Number(serverStatusTxnStats["totalAborted"]);
        let open = Number(serverStatusTxnStats["currentOpen"]);
        let started = Number(serverStatusTxnStats["totalStarted"]);
        return (active >= 0) && (inactive >= 0) && (committed >= 0) && (aborted >= 0) &&
            (open >= 0) && (started >= 0);
    }

    /**
     * serverStatus invariant: totalPreparedThenAborted + totalPreparedThenCommitted +
     * currentPrepared = totalPrepared
     */
    function preparedAbortedPlusPreparedCommittedPlusCurrentPreparedEqualsTotalPrepared(
        serverStatusTxnStats) {
        let preparedAborted = Number(serverStatusTxnStats["totalPreparedThenAborted"]);
        let preparedCommitted = Number(serverStatusTxnStats["totalPreparedThenCommitted"]);
        let currentPrepared = Number(serverStatusTxnStats["currentPrepared"]);
        let totalPrepared = Number(serverStatusTxnStats["totalPrepared"]);
        return (preparedAborted + preparedCommitted + currentPrepared) === totalPrepared;
    }

    /**
     * Certain metrics for non-prepared transactions can be calculated by subtracting the relevant
     * total transactions metric by the relevant prepared transactions metric.
     * serverStatus invariant: unpreparedAborted + unpreparedCommitted + unpreparedOpen =
     * totalUnprepared
     */
    function unpreparedAbortedPlusUnpreparedCommittedPlusUnpreparedOpenEqualsTotalUnprepared(
        serverStatusTxnStats) {
        let unpreparedAborted = Number(serverStatusTxnStats["totalAborted"]) -
            Number(serverStatusTxnStats["totalPreparedThenAborted"]);
        let unpreparedCommitted = Number(serverStatusTxnStats["totalCommitted"]) -
            Number(serverStatusTxnStats["totalPreparedThenCommitted"]);
        let unpreparedOpen = Number(serverStatusTxnStats["currentOpen"]) -
            Number(serverStatusTxnStats["currentPrepared"]);
        let totalUnprepared = Number(serverStatusTxnStats["totalStarted"]) -
            Number(serverStatusTxnStats["totalPrepared"]);
        return (unpreparedAborted + unpreparedCommitted + unpreparedOpen) === totalUnprepared;
    }

    /**
     * Checks that the invariant described by 'predFn' holds for the given samples, with a
     * maximum error of maxErrPct.
     */
    function checkInvariant(samples, predFn, maxErrPct) {
        let failedSamples = filterFalse(samples, predFn);
        let errRate = failedSamples.length / samples.length;
        assertAlways.lte(errRate, maxErrPct, () => {
            let failedSamplesStr = failedSamples.map(tojsononeline).join("\n");
            return "'" + predFn.name + "' invariant violated. Failed samples: " + failedSamplesStr;
        });
    }

    /**
     * Check invariants of transactions metrics reported in 'serverStatus' (server-wide metrics),
     * using the number of given samples.
     *
     * Inside the server, these metrics are tracked individually with atomic counters, but there
     * is no guarantee that two separate counters are updated atomically. There may be a delay
     * between the update of one counter (e.g. 'currentOpen') and another counter (e.g.
     * 'totalAborted'). This means that some invariants may not strictly hold at all times. The
     * assumption is that when these invariants are broken due to these non atomic updates, they
     * are broken for an extremely short period of time, and therefore only appear very rarely
     * when sampling the output of these metrics. We base the testing strategy below on this
     * assumption. Instead of asserting that a particular invariant holds 100% of the time, we
     * assert something slightly weaker i.e. that the invariant holds, for example, 95% percent
     * of the time. The error bounds for this test were determined somewhat empirically, but
     * they were kept very conservative. One goal of these tests is to ensure that if a change
     * was made that broke these metrics significantly, it would be picked up by these tests.
     * This test should not be sensitive to small fluctuations in metrics output.
     */
    function checkServerStatusInvariants(db, nSamples) {
        // Sample serverStatus several times, sleeping a bit in between.
        let samples = [];
        for (let i = 0; i < nSamples; ++i) {
            let txnStats = db.adminCommand({serverStatus: 1}).transactions;
            samples.push(txnStats);
            sleep(50);  // milliseconds.
        }

        // We consider an invariant failure rate of 5% within a large enough sample to be acceptable
        // For example, in a batch of 100 metrics samples, we would accept <= 5 violations of a
        // particular invariant.
        const maxErrPct = 0.05;

        checkInvariant(samples, activePlusInactiveEqualsOpen, maxErrPct);
        checkInvariant(samples, committedPlusAbortedPlusOpenEqualsStarted, maxErrPct);
        checkInvariant(samples,
                       preparedAbortedPlusPreparedCommittedPlusCurrentPreparedEqualsTotalPrepared,
                       maxErrPct);
        checkInvariant(
            samples,
            unpreparedAbortedPlusUnpreparedCommittedPlusUnpreparedOpenEqualsTotalUnprepared,
            maxErrPct);

        // allCountsNonNegative() is always expected to succeed.
        checkInvariant(samples, allCountsNonNegative, 0);
    }

    $config.setup = function(db, collName, cluster) {
        $super.setup.apply(this, arguments);
        this.prepareProbability = 0.5;
    };

    $config.teardown = function(db, collName, cluster) {
        // Check the server-wide invariants one last time, with only a single sample, since all user
        // operations should have finished.
        checkServerStatusInvariants(db, 1);
        $super.teardown.apply(this, arguments);
    };

    $config.states.checkInvariants = function checkInvariants(db, collName) {
        // Check server-wide invariants using 100 samples. This sample size is deemed big enough to
        // account for transient inconsistencies, which we assume are rare.
        let nSamples = 100;
        checkServerStatusInvariants(db, nSamples);

        // Check currentOp metrics invariants for all running transactions. These timing related
        // invariants are expected to always hold.
        let currentOp = db.currentOp({"transaction": {$exists: true}});
        currentOp.inprog.forEach((op) => {
            let txnStats = op.transaction;
            let timeActive = Number(txnStats["timeActiveMicros"]);
            let timeInactive = Number(txnStats["timeInactiveMicros"]);
            let timeOpen = Number(txnStats["timeOpenMicros"]);
            assertAlways.eq(timeActive + timeInactive, timeOpen, () => tojson(txnStats));
        });
    };

    $config.transitions = {
        init: {update: 0.9, checkInvariants: 0.1},
        update: {update: 0.8, checkInvariants: 0.1, causalRead: 0.1},
        checkInvariants: {update: 0.9, causalRead: 0.1},
        causalRead: {update: 1.0}
    };

    return $config;
});
