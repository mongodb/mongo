/**
 * query_stats_enable_disable.js
 *
 * Stresses $queryStats being toggled on and off during execution.
 *
 * @tags: [
 *  requires_fcv_72,
 *  # Cannot read query stats entries from a different node than issued the query.
 *  does_not_support_causal_consistency,
 *  # setParameter is not persistent.
 *  does_not_support_stepdowns,
 *  # Changing the query stats parameters messes with the query stats test which expects to see
 *  # query stats results.
 *  incompatible_with_concurrency_simultaneous,
 *  requires_getmore,
 * ]
 *
 */

import {setParameterOnAllNodes} from "jstests/concurrency/fsm_workload_helpers/set_parameter.js";

export const $config = (function() {
    function setCacheSize(db, cacheSize) {
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryStatsCacheSize: cacheSize}));
    }

    function setRateLimit(db, rateLimit) {
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryStatsRateLimit: rateLimit}));
    }

    function runQueryStats({db, options}) {
        try {
            // Use a small batch size to ensure these operations open up a cursor and use
            // multiple getMores - this will better stress odd concurrency states.
            const cursor =
                db.getSiblingDB("admin").aggregate([{$queryStats: options}], {batchSize: 1});
            // Can't assert much in particular about the results.
            assert.gte(cursor.itcount(), 0);
        } catch (e) {
            if (e.code === 6579000) {
                // This means query stats is disabled, which is expected to happen in this workload.
                return;
            }
            throw e;
        }
    }

    const hmacOptions = {
        transformIdentifiers: {
            algorithm: "hmac-sha-256",
            hmacKey: BinData(8, "MjM0NTY3ODkxMDExMTIxMzE0MTUxNjE3MTgxOTIwMjE=")
        }
    };
    const nDifferentShapes = 200;
    const states = {
        // The main operating states which toggle the feature on and off, since we've seen
        // bugs like SERVER-84730 when this happens.
        disableViaCacheSize: (db, collName) => setCacheSize(db, "0MB"),
        enableViaCacheSize: (db, collName) => setCacheSize(db, "1MB"),
        disableViaRateLimit: (db, collName) => setRateLimit(db, 0),
        enableViaRateLimit: (db, collName) => setRateLimit(db, -1),

        runOneQuery: function runOneQuery(db, collName) {
            assert.eq(null,
                      db[collName].findOne({["field" + Random.randInt(nDifferentShapes)]: 42}));
        },

        runQueryStatsWithHmac: (db, collName) => runQueryStats({db: db, options: hmacOptions}),
        runQueryStatsWithoutHmac: (db, collName) => runQueryStats({db: db, options: {}}),
        init: (db, collName) => { /* no op */ },
    };

    const transitions = {
        init: {
            // Make the most common thing be running a query.
            runOneQuery: 0.6,
            // Followed by runing $queryStats, with half the probability.
            runQueryStatsWithHmac: 0.15,
            runQueryStatsWithoutHmac: 0.15,
            // These sum up to 0.1:
            disableViaCacheSize: 0.05,
            disableViaRateLimit: 0.05,
        },
        runOneQuery: {init: 1},
        runQueryStatsWithHmac: {init: 1},
        runQueryStatsWithoutHmac: {init: 1},
        // If you disable it, immediately re-enable it.
        disableViaCacheSize: {enableViaCacheSize: 1},
        enableViaCacheSize: {init: 1},
        disableViaRateLimit: {enableViaRateLimit: 1},
        enableViaRateLimit: {init: 1},
    };

    var internalQueryStatsRateLimit;
    var internalQueryStatsCacheSize;

    const setup = function(db, collName, cluster) {
        // TODO SERVER-85405 Make this pattern easier and repeated throughout multiple workloads,
        // not just the query stats ones.
        internalQueryStatsRateLimit = setParameterOnAllNodes(
            {cluster: cluster, paramName: "internalQueryStatsRateLimit", newValue: -1});
        internalQueryStatsCacheSize = setParameterOnAllNodes(
            {cluster: cluster, paramName: "internalQueryStatsCacheSize", newValue: "1MB"});
    };

    const teardown = function(db, collName, cluster) {
        setParameterOnAllNodes({
            cluster: cluster,
            paramName: "internalQueryStatsRateLimit",
            newValue: internalQueryStatsRateLimit,
            // We were messing with the settings in this test by sending commands only to the
            // primary. The secondaries are not expected to be involved and so cannot be expected to
            // have the same settings.
            assertAllSettingsWereIdentical: false,
        });
        setParameterOnAllNodes({
            cluster: cluster,
            paramName: "internalQueryStatsCacheSize",
            newValue: internalQueryStatsCacheSize,
            assertAllSettingsWereIdentical: false,
        });
    };

    return {
        threadCount: 10,
        iterations: 10,
        states: states,
        setup: setup,
        teardown: teardown,
        transitions: transitions
    };
})();
