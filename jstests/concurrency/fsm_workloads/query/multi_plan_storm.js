/**
 * This tests simulates a multi-plan storm with multiple requests with the same query shapes. The
 * queries in the test are range queries which means that a cached plan can be frequently
 * invalidated.
 * @tags: [
 *  requires_fcv_82,
 *  featureFlagMultiPlanLimiter,
 *  requires_getmore,
 *  incompatible_with_concurrency_simultaneous,
 *  assumes_stable_shard_list,
 *  does_not_support_stepdowns,
 * ]
 */
export const $config = (function() {
    const data = {
        numDocs: 20000,
        numUniqueKeys: 100,
        concurrentMultiPlanningThresholdOriginalValues: [],
        concurrentMultiPlanJobsPerCacheKey: [],
    };

    const states = {
        query: function query(db, collName) {
            const x = Random.randInt(this.numUniqueKeys);
            const y = Random.randInt(this.numUniqueKeys);
            const z = Random.randInt(this.numUniqueKeys);
            // Range queries.
            const count = db[collName]
                              .aggregate([{$match: {a: {$gt: x}, b: {$gt: y}, c: {$lt: z}}}])
                              .itcount();
        }
    };

    const transitions = {query: {query: 1}};

    function setup(db, collName, cluster) {
        function setParameter(db, parameterName, newValue, originalStorage) {
            const originalParamValue =
                assert.commandWorked(db.adminCommand({getParameter: 1, [parameterName]: 1}));
            assert.commandWorked(db.adminCommand({setParameter: 1, [parameterName]: newValue}));
            originalStorage[db.getMongo().host] = originalParamValue[parameterName];
        }

        // Forcing the multi-planning rate limiter.
        cluster.executeOnMongodNodes(db => {
            setParameter(db,
                         "internalQueryConcurrentMultiPlanningThreshold",
                         5,
                         this.concurrentMultiPlanningThresholdOriginalValues);

            setParameter(db,
                         "internalQueryMaxConcurrentMultiPlanJobsPerCacheKey",
                         15,
                         this.concurrentMultiPlanJobsPerCacheKey);
        });

        assert.commandWorked(db[collName].createIndex({a: 1}));
        assert.commandWorked(db[collName].createIndex({b: 1}));
        assert.commandWorked(db[collName].createIndex({c: 1}));

        // Load example data.
        const bulk = db[collName].initializeUnorderedBulkOp();
        for (let i = 0; i < this.numDocs; ++i) {
            bulk.insert({
                flag: i % 2 ? true : false,
                a: Random.randInt(this.numUniqueKeys),
                b: Random.randInt(this.numUniqueKeys),
                c: Random.randInt(this.numUniqueKeys),
                rand: Random.rand(),
                randInt: Random.randInt(this.numDocs)
            });
        }
        let res = bulk.execute();
        assert.commandWorked(res);
    }

    function teardown(db, collName, cluster) {
        let rateLimiterAllowedCount = 0;
        let rateLimiterDelayedCount = 0;
        let rateLimiterRefusedCount = 0;

        cluster.executeOnMongodNodes(db => {
            const rateLimiterMetrics = db.serverStatus().metrics.query.multiPlanner.rateLimiter;
            rateLimiterAllowedCount += rateLimiterMetrics.allowed;
            rateLimiterDelayedCount += rateLimiterMetrics.delayed;
            rateLimiterRefusedCount += rateLimiterMetrics.refused;

            assert.commandWorked(db.adminCommand({
                setParameter: 1,
                internalQueryConcurrentMultiPlanningThreshold:
                    this.concurrentMultiPlanningThresholdOriginalValues[db.getMongo().host]
            }));

            assert.commandWorked(db.adminCommand({
                setParameter: 1,
                internalQueryMaxConcurrentMultiPlanJobsPerCacheKey:
                    this.concurrentMultiPlanJobsPerCacheKey[db.getMongo().host]
            }));
        });

        jsTest.log(`Rate Limiter metrics: allowed=${rateLimiterAllowedCount}, delayed=${
            rateLimiterDelayedCount}, refused=${rateLimiterRefusedCount}`);

        assert.gt(rateLimiterAllowedCount, 0);
        assert.gte(rateLimiterDelayedCount, 0);
        assert.gte(rateLimiterRefusedCount, 0);
    }

    return {
        threadCount: 20,
        iterations: 2,
        states: states,
        startState: 'query',
        transitions: transitions,
        data: data,
        setup: setup,
        teardown: teardown,
    };
})();
