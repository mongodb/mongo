/**
 * Test that the plan cache hits and misses serverStatus counters are updated correctly when a plan
 * is recovered from the plan cache.
 *
 * @tags: [
 *   # Bonsai optimizer cannot use the plan cache yet.
 *   cqf_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/sbe_util.js");

const conn = MongoRunner.runMongod({});
const db = conn.getDB("plan_cache_hits_and_misses_metrics");
const coll = db.coll;
coll.drop();

const collCapped = db.coll_capped;
collCapped.drop();
assert.commandWorked(db.createCollection(collCapped.getName(), {capped: true, size: 100000}));

assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(collCapped.insert({a: 1}));

const isSbeEnabled = checkSBEEnabled(db);

/**
 * Retrieves the "hits" and "misses" serverStatus metrics for the given 'planCacheType' (sbe or
 * classic) and returns them as an object: {hits: <number>, misses: <number>}.
 */
function getPlanCacheMetrics(planCacheType) {
    const serverStatus = assert.commandWorked(db.serverStatus());
    const hits = serverStatus.metrics.query.planCache[planCacheType].hits;
    const misses = serverStatus.metrics.query.planCache[planCacheType].misses;
    return {hits, misses};
}

/**
 * Runs the given command (either find or aggregate) and validates that the "hits" and "misses"
 * serverStatus metrics for the given 'planCacheType' (sbe or classic) have been updated according
 * to our expectations described in the 'hitsShouldChange' argument. This argument is an array of
 * boolean values which provide this function with the following two details:
 *    - how many times to run the given command
 *    - how the "hits" and "misses" values are expected to change after each run (true - we expect
 *      it to increase, false - remain unchanged)
 *    - only the expectations for the "hits" metric is provided, since the "misses" value changes
 *      inversely to the "hits" metric (if "hits" is expected to be changed, than "misses" should
 *      remain unchanged, and vice versa)
 *
 * The 'planCacheType' is derived from the current mongod instance settings (either sbe or classic),
 * if not specified, and can be overwritten by providing a specific value.
 *
 * If the 'indexes' argument is provided, it must be an array of index specifications to be passed
 * to 'coll.createIndexes' command, before executing the given command. All indexes previously
 * defined on the collection will be dropped. An array can be empty, in which case indexes will be
 * dropped and no new indexes will be created.
 */
function runCommandAndCheckPlanCacheMetric(
    {command, indexes, hitsShouldChange, planCacheType = (isSbeEnabled ? "sbe" : "classic")}) {
    if (indexes) {
        assert.commandWorked(coll.dropIndexes());

        if (indexes.length > 0) {
            assert.commandWorked(db.coll.createIndexes(indexes));
        }
    }

    hitsShouldChange.forEach(function(shouldChange) {
        const oldMetrics = getPlanCacheMetrics(planCacheType);
        assert.commandWorked(db.runCommand(command));
        const newMetrics = getPlanCacheMetrics(planCacheType);

        const checkMetrics = function(metricShouldChange, metric) {
            const increment = metricShouldChange ? 1 : 0;
            assert.eq(newMetrics[metric], oldMetrics[metric] + increment, command);
        };

        checkMetrics(shouldChange, "hits");
        checkMetrics(!shouldChange, "misses");
    });
}

// Run test cases.
[
    // A simple collection scan. We should only recover from plan cache when SBE is on.
    {
        command: {find: coll.getName(), filter: {a: 1}, comment: "query coll scan"},
        hitsShouldChange: [false, isSbeEnabled]
    },
    // Same as above but with an aggregate command.
    {
        command: {
            aggregate: coll.getName(),
            pipeline: [{$match: {a: 1}}],
            cursor: {},
            comment: "query coll scan aggregate"
        },
        hitsShouldChange: [isSbeEnabled]
    },
    // Same query but with two indexes on the collection. We should recover from plan cache on
    // third run when a plan cache entry gets activated.
    {
        command: {find: coll.getName(), filter: {a: 1}, comment: "query two indexes"},
        indexes: [{a: 1}, {a: 1, b: 1}],
        hitsShouldChange: [false, false, true]
    },
    // Same query shape as above, should always recover from plan cache.
    {
        command:
            {find: coll.getName(), filter: {a: 5}, comment: "query two indexes different eq cost"},
        hitsShouldChange: [true],
    },
    // Same query as above, but with an aggregate command. Should always recover from plan cache.
    {
        command: {
            aggregate: coll.getName(),
            pipeline: [{$match: {a: 5}}],
            cursor: {},
            comment: "query two indexes aggregate"
        },
        hitsShouldChange: [true],
    },
    // IdHack queries is always is executed with the classic engine and never get cached.
    {
        command: {find: coll.getName(), filter: {_id: 1}, comment: "query idhack"},
        hitsShouldChange: [false, false, false],
        planCacheType: "classic"
    },
    // Hinted queries are cached and can be recovered only in SBE. Note that 'hint' changes the
    // query shape, so we expect to recover only on a second run.
    {
        command: {find: coll.getName(), filter: {a: 1}, comment: "query hint", hint: {a: 1}},
        hitsShouldChange: [false, isSbeEnabled],
    },
    // Min queries never get cached.
    {
        command: {
            find: coll.getName(),
            filter: {a: 1},
            comment: "query min",
            min: {a: 10},
            hint: {a: 1}
        },
        hitsShouldChange: [false, false, false],
    },
    // Max queries never get cached.
    {
        command: {
            find: coll.getName(),
            filter: {a: 1},
            comment: "query max",
            max: {a: 10},
            hint: {a: 1}
        },
        hitsShouldChange: [false, false, false],
    },
    // We don't cache plans for explain.
    {
        command: {explain: {find: coll.getName(), filter: {a: 1}, comment: "query explain"}},
        hitsShouldChange: [false, false, false],
    },
    // Tailable cursor queries never get cached.
    {
        command:
            {find: collCapped.getName(), filter: {a: 1}, comment: "query tailable", tailable: true},
        hitsShouldChange: [false, false, false],
    },
].forEach(testCase => runCommandAndCheckPlanCacheMetric(testCase));

MongoRunner.stopMongod(conn);
})();
