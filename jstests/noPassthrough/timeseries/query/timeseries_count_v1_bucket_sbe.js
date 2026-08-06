/**
 * Tests that $count / {$sum: 1} over a time-series collection returns correct results, and runs
 * on SBE, when the collection contains a version-1 (uncompressed) bucket. The count/group
 * bucket-rewrite falls back to {$size: {$objectToArray: ...}} for uncompressed buckets, and that
 * $size call must execute correctly under SBE (SERVER-80698).
 *
 * New buckets are always created compressed (version 3), so this test writes a raw version-1
 * bucket directly to exercise the fallback branch.
 */

import {getEngine} from "jstests/libs/query/analyze_plan.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const conn = MongoRunner.runMongod({
    setParameter: {internalQueryFrameworkControl: "trySbeEngine"},
});
const db = conn.getDB(jsTestName());
const coll = db.coll;

assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}),
);

const t = ISODate("2024-01-16T20:48:00Z");
const uncompressedBucket = {
    _id: ObjectId("65a6eb806ffc9fa4280ecac4"),
    control: {
        version: 1,
        min: {_id: ObjectId("65a6eba7e6d2e848e08c3750"), t: t, a: 0},
        max: {_id: ObjectId("65a6eba7e6d2e848e08c3752"), t: t, a: 2},
    },
    meta: 0,
    data: {
        _id: {
            0: ObjectId("65a6eba7e6d2e848e08c3750"),
            1: ObjectId("65a6eba7e6d2e848e08c3751"),
            2: ObjectId("65a6eba7e6d2e848e08c3752"),
        },
        t: {0: t, 1: t, 2: t},
        a: {0: 0, 1: 1, 2: 2},
    },
};

assert.commandWorked(
    getTimeseriesCollForRawOps(db, coll).insertOne(uncompressedBucket, getRawOperationSpec(db)),
);
assert.eq(coll.find().itcount(), 3, "expected 3 unpacked measurements from the raw v1 bucket");

for (const pipeline of [[{$count: "n"}], [{$group: {_id: null, n: {$sum: 1}}}]]) {
    const explain = coll.explain().aggregate(pipeline);
    assert.eq(
        getEngine(explain),
        "sbe",
        () => `expected pipeline ${tojson(pipeline)} to run on SBE. Explain: ${tojson(explain)}`,
    );

    const result = coll.aggregate(pipeline).toArray();
    assert.eq(result.length, 1, () => `unexpected result shape: ${tojson(result)}`);
    assert.eq(
        result[0].n,
        3,
        () => `pipeline ${tojson(pipeline)} returned wrong count: ${tojson(result)}`,
    );
}

MongoRunner.stopMongod(conn);
