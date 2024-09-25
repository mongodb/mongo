/**
 * query_stats_concurrent.js
 *
 * Stresses $queryStats running concurrently with queries.
 *
 * @tags: [
 *  requires_fcv_72,
 *  does_not_support_causal_consistency,
 *  requires_getmore,
 *  # TODO (SERVER-95174): Re-enable this test on txn concurrency suites.
 *  does_not_support_transactions,
 * ]
 *
 *
 */
import {setParameterOnAllNodes} from "jstests/concurrency/fsm_workload_helpers/set_parameter.js";

export const $config = (function() {
    var states = (function() {
        function init(db, collName) {
        }

        function reInit(db, collName) {
        }

        // Runs one find query so that the queryStatsEntry is updated.
        function findOneShape(db, collName) {
            assert.gt(db[collName].find({i: {$lt: 50}}).itcount(), 0);
        }

        // Runs one agg query so that the queryStatsEntry is updated.
        function aggOneShape(db, collName) {
            assert.gt(db[collName].aggregate([{$match: {i: {$gt: 900}}}]).itcount(), 0);
        }

        // Runs many queries with different shapes to ensure eviction occurs in the queryStats
        // store.
        function multipleShapes(db, collName) {
            for (var i = 0; i < 2000; i++) {
                let query = {};
                query["foo" + i] = "bar";
                db[collName].aggregate([{$match: query}]).itcount();
            }
            const evictedAfter = db.serverStatus().metrics.queryStats.numEvicted;
            assert.gt(evictedAfter, 0);
        }

        // Runs queryStats with transformation.
        function runQueryStatsWithHmac(db, collName) {
            let response = db.adminCommand({
                aggregate: 1,
                pipeline: [{
                    $queryStats: {
                        transformIdentifiers: {
                            algorithm: "hmac-sha-256",
                            hmacKey: BinData(8, "MjM0NTY3ODkxMDExMTIxMzE0MTUxNjE3MTgxOTIwMjE=")
                        }
                    }
                }],
                // Use a small batch size to ensure these operations open up a cursor and use
                // multiple getMores.
                cursor: {batchSize: 1}
            });
            assert.commandWorked(response);
            const cursor = new DBCommandCursor(db.getSiblingDB("admin"), response);
            assert.gt(cursor.itcount(), 0);
        }

        // Runs queryStats without transformation.
        function runQueryStatsWithoutHmac(db, collName) {
            let response = db.adminCommand({
                aggregate: 1,
                pipeline: [{$queryStats: {}}],
                // Use a small batch size to ensure these operations open up a cursor and use
                // multiple getMores.
                cursor: {batchSize: 1}
            });
            assert.commandWorked(response);
            const cursor = new DBCommandCursor(db.getSiblingDB("admin"), response);
            assert.gt(cursor.itcount(), 0);
        }

        return {
            init: init,
            reInit: reInit,
            findOneShape: findOneShape,
            multipleShapes: multipleShapes,
            aggOneShape: aggOneShape,
            runQueryStatsWithHmac: runQueryStatsWithHmac,
            runQueryStatsWithoutHmac: runQueryStatsWithoutHmac
        };
    })();

    var internalQueryStatsRateLimit;
    var internalQueryStatsCacheSize;

    let setup = function(db, collName, cluster) {
        internalQueryStatsRateLimit = setParameterOnAllNodes(
            {cluster: cluster, paramName: "internalQueryStatsRateLimit", newValue: -1});
        internalQueryStatsCacheSize = setParameterOnAllNodes(
            {cluster: cluster, paramName: "internalQueryStatsCacheSize", newValue: "1MB"});

        assert.commandWorked(db[collName].createIndex({i: 1}));
        const bulk = db[collName].initializeUnorderedBulkOp();
        for (let i = 0; i < 1000; ++i) {
            bulk.insert({i: i});
        }
        assert.commandWorked(bulk.execute());
    };

    let teardown = function(db, collName, cluster) {
        setParameterOnAllNodes({
            cluster: cluster,
            paramName: "internalQueryStatsRateLimit",
            newValue: internalQueryStatsRateLimit
        });
        setParameterOnAllNodes({
            cluster: cluster,
            paramName: "internalQueryStatsCacheSize",
            newValue: internalQueryStatsCacheSize
        });

        db[collName].drop();
    };

    let transitions = {
        // To start, add some $queryStats data so that it is never empty.
        init: {
            aggOneShape: 0.33,
            findOneShape: 0.33,
            multipleShapes: 0.34,
        },
        // From then on, choose evenly among all possibilities:
        reInit: {
            aggOneShape: 0.2,
            findOneShape: 0.2,
            multipleShapes: 0.2,
            runQueryStatsWithHmac: 0.2,
            runQueryStatsWithoutHmac: 0.2
        },
        findOneShape: {reInit: 1},
        multipleShapes: {reInit: 1},
        runQueryStatsWithHmac: {reInit: 1},
        runQueryStatsWithoutHmac: {reInit: 1},
        aggOneShape: {reInit: 1}
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
