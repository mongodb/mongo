/**
 * Validates that the 'cached_plans_evicted' metric behaves correctly.
 */
import {checkSbeFullFeatureFlagEnabled} from "jstests/libs/query/sbe_util.js";

const mongod = MongoRunner.runMongod({setParameter: `planCacheSize=0.1MB`});
const db = mongod.getDB("test");
const coll = db[jsTestName()];
coll.drop();

const sbePlanCacheEnabled = checkSbeFullFeatureFlagEnabled(db);

// An equality query on each field will have two candidate plans.
assert.commandWorked(coll.insert({f1: 1, f2: 1, f3: 1, f4: 1, f5: 1}));
assert.commandWorked(coll.createIndexes([
    {f1: 1},
    {f2: 1},
    {f3: 1},
    {f4: 1},
    {f5: 1},
    {f1: 1, f2: 1},
    {f2: 1, f3: 1},
    {f3: 1, f4: 1},
    {f4: 1, f5: 1},
    {f5: 1, f1: 1}
]));

function getCachedPlansEvictedMetric() {
    const metricName = sbePlanCacheEnabled ? "sbe" : "classic";
    return assert.commandWorked(db.serverStatus())
        .metrics.query.planCache[metricName]
        .cached_plans_evicted;
}

function runQueriesOnField(field) {
    for (const predicate of [{[field]: 1},
                             {[field]: {$lte: 1}},
                             {[field]: {$gte: 1}},
                             {[field]: {$lt: 2}},
                             {[field]: {$gt: 0}}]) {
        assert.eq(1, coll.find(predicate).itcount());
        assert.eq(1, coll.find(predicate, {[field]: 1}).itcount());
        assert.eq(1, coll.find(predicate, {_id: 0, [field]: 1}).itcount());
        assert.eq(1, coll.find(predicate, {_id: 1, [field]: 1}).itcount());
        assert.eq(1, coll.find(predicate, {_id: 1}).itcount());
    }
}

for (let i = 1; i <= 5; i++) {
    runQueriesOnField("f" + i);
}

if (sbePlanCacheEnabled) {
    assert.gt(getCachedPlansEvictedMetric(), 1);
} else {
    // Classic plan cache doesn't store full plans so each entry takes very little space. Hence we
    // don't expect to see any evictions.
    assert.eq(getCachedPlansEvictedMetric(), 0);
}

MongoRunner.stopMongod(mongod);
