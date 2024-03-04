/**
 * Test that the plan cache hits, misses and skipped serverStatus' counters are updated correctly
 * when serving queries.
 *
 * @tags: [
 *   # TODO SERVER-85728: Enable Bonsai plan cache tests involving indices.
 *   cqf_incompatible,
 * ]
 */
import {checkSbeFullyEnabled} from "jstests/libs/sbe_util.js";

const conn = MongoRunner.runMongod({});
const db = conn.getDB("plan_cache_hits_and_misses_metrics");
const coll = db.coll;
coll.drop();

const collCapped = db.coll_capped;
collCapped.drop();
assert.commandWorked(db.createCollection(collCapped.getName(), {capped: true, size: 100000}));

assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(collCapped.insert({a: 1}));

const isSbeEnabled = checkSbeFullyEnabled(db);

/**
 * Retrieves the "hits", "misses" and "skipped" serverStatus metrics for the given 'planCacheType'
 * (sbe or classic) and returns them as an object: {hits: <number>, misses: <number>, skipped:
 * <number>}.
 */
function getPlanCacheMetrics(planCacheType) {
    const serverStatus = assert.commandWorked(db.serverStatus());
    const hits = serverStatus.metrics.query.planCache[planCacheType].hits;
    const misses = serverStatus.metrics.query.planCache[planCacheType].misses;
    const skipped = serverStatus.metrics.query.planCache[planCacheType].skipped;
    return {hits, misses, skipped};
}

// Enum for the expected cache behavior when running a particular command.
const cacheBehavior = Object.freeze({skip: 0, miss: 1, hit: 2});

/**
 * Runs the given command (either find or aggregate) and validates that the "hits", "misses" and
 * "skipped" serverStatus metrics for the given 'planCacheType' (sbe or classic) have been updated
 * according to our expectations described in the 'expectedCacheBehaviors' argument.
 *
 * 'expectedCacheBehaviors' argument is an array of
 * 'cacheBehavior' values which provide this function with the following two details:
 *    1. how many times to run the given command
 *    2. which of the three counters is expected to change after each run
 *
 * The 'planCacheType' is derived from the current mongod instance settings (either sbe or classic),
 * if not specified, and can be overwritten by providing a specific value.
 *
 * If the 'indexes' argument is provided, it must be an array of index specifications to be passed
 * to 'coll.createIndexes' command, before executing the given command. All indexes previously
 * defined on the collection will be dropped. An array can be empty, in which case indexes will be
 * dropped and no new indexes will be created.
 */
function runCommandAndCheckPlanCacheMetric({
    command,
    indexes,
    expectedCacheBehaviors,
    planCacheType = (isSbeEnabled ? "sbe" : "classic")
}) {
    if (indexes) {
        assert.commandWorked(coll.dropIndexes());

        if (indexes.length > 0) {
            assert.commandWorked(db.coll.createIndexes(indexes));
        }
    }

    expectedCacheBehaviors.forEach((expectedCacheBehavior) => {
        const oldMetrics = getPlanCacheMetrics(planCacheType);
        assert.commandWorked(db.runCommand(command));
        const newMetrics = getPlanCacheMetrics(planCacheType);

        switch (expectedCacheBehavior) {
            case cacheBehavior.skip:
                assert.eq(oldMetrics.hits, newMetrics.hits, command);
                assert.eq(oldMetrics.misses, newMetrics.misses, command);
                assert.eq(oldMetrics.skipped + 1, newMetrics.skipped, command);
                break;
            case cacheBehavior.miss:
                assert.eq(oldMetrics.hits, newMetrics.hits, command);
                assert.eq(oldMetrics.misses + 1, newMetrics.misses, command);
                assert.eq(oldMetrics.skipped, newMetrics.skipped, command);
                break;
            case cacheBehavior.hit:
                assert.eq(oldMetrics.hits + 1, newMetrics.hits, command);
                assert.eq(oldMetrics.misses, newMetrics.misses, command);
                assert.eq(oldMetrics.skipped, newMetrics.skipped, command);
                break;
            default:
                assert(false,
                       "Unknown cache behavior: " + expectedCacheBehavior +
                           " Command: " + JSON.stringify(command));
        }
    });
}

// Run test cases.
[
    // A simple collection scan. We should only recover from plan cache when SBE is on.
    {
        command: {find: coll.getName(), filter: {a: 1}, comment: "query coll scan"},
        expectedCacheBehaviors:
            [cacheBehavior.miss, isSbeEnabled ? cacheBehavior.hit : cacheBehavior.miss]
    },
    // Same as above but with an aggregate command.
    {
        command: {
            aggregate: coll.getName(),
            pipeline: [{$match: {a: 1}}],
            cursor: {},
            comment: "query coll scan aggregate"
        },
        expectedCacheBehaviors: [isSbeEnabled ? cacheBehavior.hit : cacheBehavior.miss]
    },
    // Same query but with two indexes on the collection. We should recover from plan cache on
    // third run when a plan cache entry gets activated.
    {
        command: {find: coll.getName(), filter: {a: 1}, comment: "query two indexes"},
        indexes: [{a: 1}, {a: -1}],
        expectedCacheBehaviors: [cacheBehavior.miss, cacheBehavior.miss, cacheBehavior.hit]
    },
    // Same query shape as above, should always recover from plan cache.
    {
        command:
            {find: coll.getName(), filter: {a: 5}, comment: "query two indexes different eq cost"},
        expectedCacheBehaviors: [cacheBehavior.hit],
    },
    // Same query as above, but with an aggregate command. Should always recover from plan cache.
    {
        command: {
            aggregate: coll.getName(),
            pipeline: [{$match: {a: 5}}],
            cursor: {},
            comment: "query two indexes aggregate"
        },
        expectedCacheBehaviors: [cacheBehavior.hit],
    },
    // IdHack queries is always is executed with the classic engine and never get cached.
    {
        command: {find: coll.getName(), filter: {_id: 1}, comment: "query idhack", batchSize: 200},
        expectedCacheBehaviors: [cacheBehavior.skip, cacheBehavior.skip, cacheBehavior.skip],
        planCacheType: "classic"
    },
    // Hinted queries are cached and can be recovered only in SBE. Note that 'hint' changes the
    // query shape, so we expect to recover only on a second run.
    {
        command: {find: coll.getName(), filter: {a: 1}, comment: "query hint", hint: {a: 1}},
        expectedCacheBehaviors: [
            isSbeEnabled ? cacheBehavior.miss : cacheBehavior.skip,
            isSbeEnabled ? cacheBehavior.hit : cacheBehavior.skip
        ],
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
        expectedCacheBehaviors: [cacheBehavior.skip, cacheBehavior.skip, cacheBehavior.skip],
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
        expectedCacheBehaviors: [cacheBehavior.skip, cacheBehavior.skip, cacheBehavior.skip],
    },
    // We don't cache plans for explain.
    {
        command: {explain: {find: coll.getName(), filter: {a: 1}, comment: "query explain"}},
        expectedCacheBehaviors: [cacheBehavior.skip, cacheBehavior.skip, cacheBehavior.skip],
    },
    // Tailable cursor queries never get cached.
    {
        command:
            {find: collCapped.getName(), filter: {a: 1}, comment: "query tailable", tailable: true},
        expectedCacheBehaviors: [cacheBehavior.skip, cacheBehavior.skip, cacheBehavior.skip],
    },
    // Trivially false queries never get cached.
    {
        command:
            {find: coll.getName(), filter: {"$alwaysFalse": 1}, comment: "trivially false query"},
        expectedCacheBehaviors: [cacheBehavior.skip],
    },
    // Queries on non existing collections never get cached.
    {
        command: {find: "non_existing_collection", filter: {a: 1}, comment: "non existing coll"},
        expectedCacheBehaviors: [cacheBehavior.skip],
    },
].forEach(testCase => runCommandAndCheckPlanCacheMetric(testCase));

MongoRunner.stopMongod(conn);
