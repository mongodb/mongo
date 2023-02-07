'use strict';

/**
 * Provides helpers for configuring the balancer.
 *
 * Intended for use by workloads testing sharding (i.e., workloads starting with 'sharded_').
 */

var BalancerHelper = (function() {
    // Disables balancing for a given collection.
    function disableBalancerForCollection(db, ns) {
        assertAlways.commandWorked(
            db.getSiblingDB('config').collections.update({_id: ns}, {$set: {"noBalance": true}}));
    }

    // Enables balancing for a given collection.
    function enableBalancerForCollection(db, ns) {
        assertAlways.commandWorked(
            db.getSiblingDB('config').collections.update({_id: ns}, {$unset: {"noBalance": 1}}));
    }

    // Joins the ongoing balancer round (if enabled at all).
    function joinBalancerRound(db, timeout) {
        timeout = timeout || 60000;

        var initialStatus = db.adminCommand({balancerStatus: 1});
        var currentStatus;
        assert.soon(function() {
            currentStatus = db.adminCommand({balancerStatus: 1});
            if (currentStatus.mode === 'off') {
                // Balancer is disabled.
                return true;
            }
            if (!friendlyEqual(currentStatus.term, initialStatus.term)) {
                // A new primary of the csrs has been elected
                initialStatus = currentStatus;
                return false;
            }
            assert.gte(
                currentStatus.numBalancerRounds,
                initialStatus.numBalancerRounds,
                'Number of balancer rounds moved back in time unexpectedly. Current status: ' +
                    tojson(currentStatus) + ', initial status: ' + tojson(initialStatus));
            return currentStatus.numBalancerRounds > initialStatus.numBalancerRounds;
        }, 'Latest balancer status: ' + tojson(currentStatus), timeout);
    }

    return {
        disableBalancerForCollection: disableBalancerForCollection,
        enableBalancerForCollection: enableBalancerForCollection,
        joinBalancerRound: joinBalancerRound,
    };
})();
