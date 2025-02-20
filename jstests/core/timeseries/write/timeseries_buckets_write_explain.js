/**
 * Tests explaining write operations on a time-series buckets collection.
 *
 * @tags: [
 *   featureFlagRawDataCrudOperations,
 *   requires_timeseries,
 * ]
 */

const coll = db[jsTestName()];
const bucketsColl = db["system.buckets." + coll.getName()];

const timeField = "t";
const metaField = "m";
const time = new Date("2024-01-01T00:00:00Z");

coll.drop();
assert.commandWorked(db.createCollection(
    coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}}));

assert.commandWorked(coll.insert([
    {[timeField]: time, [metaField]: 1, a: "a"},
    {[timeField]: time, [metaField]: 2, a: "b"},
    {[timeField]: time, [metaField]: 2, a: "c"},
]));

const assertExplain = function(explain) {
    if (explain.shards) {
        for (const shardExplain of Object.values(explain.shards)) {
            assert.eq(shardExplain.queryPlanner.namespace, bucketsColl.getFullName());
        }
    } else if (explain.queryPlanner.namespace) {
        assert.eq(explain.queryPlanner.namespace, bucketsColl.getFullName());
    } else {
        for (const shardPlan of explain.queryPlanner.winningPlan.shards) {
            assert.eq(shardPlan.namespace, bucketsColl.getFullName());
        }
    }
};

assertExplain(bucketsColl.explain().findAndModify(
    {query: {"control.count": 2}, update: {$set: {meta: "3"}}}));
assertExplain(bucketsColl.explain().remove({"control.count": 2}));
assertExplain(bucketsColl.explain().update({"control.count": 1}, {$set: {meta: "3"}}));
