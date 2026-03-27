/**
 * Tests the deferred engine choice get_executor path for queries that share a cache
 * entry but switch between the selected engine. Assert the correctness of the queries
 * and that replan counters are updated.
 *
 * @tags: [
 *   requires_profiling,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {getEngine, getPlanStage} from "jstests/libs/query/analyze_plan.js";
import {assertCacheUsage, setUpActiveCacheEntry} from "jstests/libs/query/plan_cache_utils.js";
import {checkSbeFullyEnabled, checkSbeCompletelyDisabled} from "jstests/libs/query/sbe_util.js";

const conn = MongoRunner.runMongod({setParameter: {featureFlagGetExecutorDeferredEngineChoice: true}});
const db = conn.getDB("test");

// This test requires us to use classic sometimes, which SBE full overrides.
if (checkSbeFullyEnabled(db) || checkSbeCompletelyDisabled(db)) {
    jsTest.log("Exiting early because SBE is fully enabled or forceClassicEngine is set.");
    MongoRunner.stopMongod(conn);
    quit();
}

const coll = db.deferred_engine_replan_engine_switch;
assert(coll.drop());

// Configure the failpoint that chooses classic if the index name is 'classic' and chooses
// SBE if the index name is 'sbe'.
const fp = configureFailPoint(conn, "engineSelectionOverrideByIndexName");
assert.commandWorked(coll.createIndex({a: 1}, {name: "classic"}));
assert.commandWorked(coll.createIndex({b: 1}, {name: "sbe"}));

const query = [{$match: {a: 0, b: 0}}, {$group: {_id: null, a_max: {$max: "$a"}}}];

const dataPreferringIndexA = [];
const dataPreferringIndexB = [];
for (let i = 0; i < 1000; i++) {
    dataPreferringIndexA.push({a: i, b: 0});
    dataPreferringIndexB.push({a: 0, b: i});
}

function assertEngineUsed(expectedEngine) {
    const cache = coll.getPlanCache().list();
    assert.eq(cache.length, 1);
    const entry = cache[0];
    const ixscan = getPlanStage(entry.cachedPlan, "IXSCAN");
    assert.eq(ixscan.indexName, expectedEngine);

    const explain = coll.explain().aggregate(query);
    assert.eq(getEngine(explain), expectedEngine);
}

function getExpectedResults(docs) {
    return db.aggregate([{$documents: docs}, ...query]).toArray();
}

function getReplannedMetric() {
    return assert.commandWorked(db.serverStatus()).metrics.query.planCache.classic.replanned;
}

function setEngine(engine) {
    const data = engine === "classic" ? dataPreferringIndexA : dataPreferringIndexB;
    assert(coll.remove({}));
    assert.commandWorked(coll.insert(data));
    const results = coll.aggregate(query).toArray();
    assertEngineUsed(engine);
    assert.eq(results, getExpectedResults(data, query));
    // Run the query again to activate the cache entry.
    coll.aggregate(query).toArray();
    coll.aggregate(query).toArray();
}

// Setup a classic cache entry, then replan a few times to switch back and forth between SBE and classic.
setEngine("classic");
for (let i = 0; i < 5; i++) {
    const originalReplanCount = getReplannedMetric();
    setEngine("sbe");
    assert.eq(originalReplanCount + 1, getReplannedMetric());
    setEngine("classic");
    assert.eq(originalReplanCount + 2, getReplannedMetric());
}

fp.off();
MongoRunner.stopMongod(conn);
