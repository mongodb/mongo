/**
 * Tests that if there is a CBR plan in the cache but CBR is disabled,
 * queries on that collection and other collections succeed.
 *
 * @tags: [
 *   # We need enough data per shard to trigger the CBR fallback. When a
 *   # collection is implicitly sharded, documents are split across shards and CBR may not run on
 *   # every shard. This test therefore requires an unsharded collection.
 *   assumes_unsharded_collection,
 *   # Refusing to run a test that issues commands that may return different values after a failover
 *   does_not_support_stepdowns,
 *   # Asserts on plan cache entries
 *   assumes_no_implicit_index_creation,
 *   # This test relies on $planCacheStats which must be the first stage in the pipeline
 *   exclude_from_timeseries_crud_passthrough,
 *   # featureFlagCostBasedRanker was introduced in 8.3
 *   requires_fcv_83,
 *   # Aggregation stage $planCacheStats cannot run within a multi-document transaction.
 *   does_not_support_transactions,
 *   # moveCollection (used by random_migrations suites) recreates the collection on the destination
 *   # shard with an empty plan cache, which would cause plan cache assertions to fail mid-test.
 *   assumes_stable_collection_uuid,
 *   # TODO(SERVER-124265): Remove.
 *   featureFlagReplicatedFastCount_incompatible,
 * ]
 */

import {getCachedPlanForQuery} from "jstests/libs/query/analyze_plan.js";
import {getCBRConfig, setCBRConfigOnAllNonConfigNodes} from "jstests/libs/query/cbr_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

if (checkSbeFullyEnabled(db)) {
    jsTest.log.info("Skipping test because the SBE plan cache is enabled");
    quit();
}

const collName = jsTestName();
const coll = db[collName];
const aIndexQuery = {a: {$gte: 100}, b: {$gte: 1}, c: 1};

const otherCollName = collName + "_other";
const otherColl = db[otherCollName];
const eIndexQuery = {d: 1, e: 5};

const kNumDocs = 15000;
const baseDocs = [];
for (let i = 0; i < kNumDocs; i++) {
    baseDocs.push({a: i, b: i});
}
baseDocs.push({a: 7001, b: 7001, c: 1});
baseDocs.push({a: 8001, b: 8001, c: 1});

// Verify the plan cache entry for 'query' on 'targetColl', asserting whether or not CBR chose the
// cached plan based on 'pickedByCBR'.
function checkPlanCacheForQuery(targetColl, query, pickedByCBR) {
    const entry = getCachedPlanForQuery(db, targetColl, query);

    assert.eq(true, entry.isActive, entry);

    // Assert that CBR chose the plan that has been cached.
    // TODO SERVER-116684: Right now we do this by knowing that the
    // $planCacheStats output for CBR plans is incomplete and does not
    // show rejected plans (i.e. there are two indexes for consideration
    // here, but there's only 1 entry in the creationExecStats array).
    // Once we fix $planCacheStats, we should change these assertions.
    assert.eq(entry.creationExecStats.length, pickedByCBR ? 1 : 2, entry);
    assert.eq(entry.candidatePlanScores.length, pickedByCBR ? 1 : 2, entry);
}

const prevCBRConfig = getCBRConfig(db);

try {
    // Ensure CBR is enabled.
    setCBRConfigOnAllNonConfigNodes(db, {featureFlagCostBasedRanker: true});

    coll.drop();
    const docs = [...baseDocs];
    assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));

    // Used to create another cache entry for this collection. Create these indexes now so that
    // doing so later does not clear the plan cache.
    for (let i = 0; i < 10; i++) {
        docs.push({d: 1, e: i}); // 'e' is more selective.
    }
    assert.commandWorked(coll.createIndexes([{d: 1}, {e: 1}]));
    assert.commandWorked(coll.insertMany(docs));

    coll.getPlanCache().clear();

    // Create inactive plan cache entry, then activate it with second query.
    coll.find(aIndexQuery).toArray();
    coll.find(aIndexQuery).toArray();

    checkPlanCacheForQuery(coll, aIndexQuery, true /* pickedByCBR */);

    // Disable CBR and run a followup query.
    setCBRConfigOnAllNonConfigNodes(db, {featureFlagCostBasedRanker: false});

    // This query would use the existing plan cache entry. Assert that it succeeds.
    let res = coll.find(aIndexQuery).toArray();
    assert.eq(res.length, 2, res);

    // The same plan should be in the cache even after CBR is disabled and it was used.
    checkPlanCacheForQuery(coll, aIndexQuery, true /* pickedByCBR */);

    // Create and use a new cache entry for a different query, not chosen by CBR.
    for (let i = 0; i < 3; i++) {
        res = coll.find(eIndexQuery).toArray();
        assert.eq(1, res.length, res);
    }
    checkPlanCacheForQuery(coll, eIndexQuery, false /* pickedByCBR */);

    // Check that the cache entry for the first query is still there.
    checkPlanCacheForQuery(coll, aIndexQuery, true /* pickedByCBR */);

    // Create another collection and ensure a query against it succeeds.
    otherColl.drop();
    otherColl.insertOne({x: 1});
    res = otherColl.find({x: 1}).toArray();
    assert.eq(res.length, 1, res);

    // Trigger replanning of the CBR-chosen plan from above by modifying the data and assert that
    // a query with the same shape succeeds.
    const extraDocs = [];
    for (let i = 0; i < 100000; i++) {
        extraDocs.push({a: 500, b: 500});
    }
    assert.commandWorked(coll.insertMany(extraDocs));

    // Run 3 times: the first run detects the stale cached plan and triggers replanning, creating a
    // new inactive entry; the second run increases the works stored in the cache; the third activates it.
    let currWorks = 0;
    for (let i = 0; i < 3; i++) {
        res = coll.find(aIndexQuery).toArray();
        assert.eq(res.length, 2, res);

        const entry = getCachedPlanForQuery(db, coll, aIndexQuery);
        if (i < 2) {
            // We only expect the works to increase on the runs where the cached entry is inactive.
            assert.gt(entry.works, currWorks, `Works did not increase: ${tojson(entry)}`);
            assert(!entry.isActive, `Incorrect active-ness state: ${tojson(entry)}`);
            currWorks = entry.works;
        } else {
            assert(entry.isActive, `Incorrect active-ness state: ${tojson(entry)}`);
        }
    }

    // Verify that there's a new cache entry for the query, but CBR did not pick the new plan.
    checkPlanCacheForQuery(coll, aIndexQuery, false /* pickedByCBR */);

    // Test that dropping an index invalidates a CBR-chosen plan cache entry.
    const dropTestColl = db[collName + "_drop"];
    dropTestColl.drop();
    assert.commandWorked(dropTestColl.createIndexes([{a: 1}, {b: 1}]));
    assert.commandWorked(dropTestColl.insertMany(baseDocs));

    // Enable CBR and populate the cache with a CBR-chosen plan for aIndexQuery.
    setCBRConfigOnAllNonConfigNodes(db, {featureFlagCostBasedRanker: true});
    dropTestColl.find(aIndexQuery).toArray();
    dropTestColl.find(aIndexQuery).toArray();

    checkPlanCacheForQuery(dropTestColl, aIndexQuery, true /* pickedByCBR */);

    // Disable CBR before dropping the index.
    setCBRConfigOnAllNonConfigNodes(db, {featureFlagCostBasedRanker: false});

    // Drop one of the indexes, resulting in the entire plan cache clearing.
    assert.commandWorked(dropTestColl.dropIndex({a: 1}));

    const planCacheAfterDrop = dropTestColl.getPlanCache().list();
    assert.eq(
        0,
        planCacheAfterDrop.length,
        `Expected empty plan cache after index drop: ${tojson(planCacheAfterDrop)}`,
    );

    // Queries must still succeed.
    res = dropTestColl.find(aIndexQuery).toArray();
    assert.eq(2, res.length, res);
} finally {
    setCBRConfigOnAllNonConfigNodes(db, prevCBRConfig);
}
