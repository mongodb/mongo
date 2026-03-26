/**
 * Validates that $planCacheStats works on a viewful time-series collection.
 *
 * @tags: [
 *   requires_fcv_83,
 *   requires_timeseries,
 * ]
 */
import {getPlanCacheKeyFromShape} from "jstests/libs/query/analyze_plan.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod failed to start up");

try {
    const db = conn.getDB(jsTestName() + "_db");
    const coll = db.timeseries_plan_cache_stats;
    coll.drop();

    const timeField = "timeField";
    const metaField = "metaFeild";

    assert.commandWorked(
        db.createCollection(coll.getName(), {
            timeseries: {timeField: timeField, metaField: metaField},
        }),
    );

    assert.commandWorked(coll.createIndex({[metaField]: 1}));

    assert.commandWorked(
        coll.insertMany([
            {[timeField]: ISODate("2024-03-25T00:00:00Z"), [metaField]: "first", v: 1},
            {[timeField]: ISODate("2025-03-25T00:00:00Z"), [metaField]: "first", v: 2},
            {[timeField]: ISODate("2026-03-25T00:00:00Z"), [metaField]: "second", v: 3},
        ]),
    );

    const query = {[timeField]: {$gt: ISODate("2020-03-25T00:00:00Z")}, [metaField]: "first"};

    const planCacheKey = getPlanCacheKeyFromShape({
        query,
        collection: coll,
        db,
    });

    assert.eq(coll.find(query).itcount(), 2);

    const entries = coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey}}]).toArray();
    assert.eq(entries.length, 1, "expected exactly one plan cache entry: " + tojson(entries));
} finally {
    MongoRunner.stopMongod(conn);
}
