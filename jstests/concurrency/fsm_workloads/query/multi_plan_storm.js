/**
 * This tests simulates a multi-plan storm with multiple requests with the same query shapes. The
 * queries in the test are range queries which means that a cached plan can be frequently
 * invalidated.
 * @tags: [
 *  requires_fcv_82,
 *  requires_getmore,
 *  incompatible_with_concurrency_simultaneous,
 * ]
 */
export const $config = (function() {
    const data = {
        numDocs: 20000,
        numUniqueKeys: 100,
        concurrentMultiPlanningThresholdOriginalValues: [],
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
        // Forcing the multi-planning rate limiter.
        cluster.executeOnMongodNodes(db => {
            const originalParamValue = assert.commandWorked(db.adminCommand(
                {getParameter: 1, internalQueryConcurrentMultiPlanningThreshold: 1}));
            assert.commandWorked(db.adminCommand(
                {setParameter: 1, internalQueryConcurrentMultiPlanningThreshold: 5}));
            this.concurrentMultiPlanningThresholdOriginalValues[db.getMongo().host] =
                originalParamValue.internalQueryConcurrentMultiPlanningThreshold;
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
        cluster.executeOnMongodNodes(db => {
            assert.commandWorked(db.adminCommand({
                setParameter: 1,
                internalQueryConcurrentMultiPlanningThreshold:
                    this.concurrentMultiPlanningThresholdOriginalValues[db.getMongo().host]
            }));
        });
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
