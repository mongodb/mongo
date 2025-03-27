/**
 * Helper for testing the behavior of $group on time-series collections.
 */

import {getAggPlanStage, getEngine, getPlanStage} from "jstests/libs/query/analyze_plan.js";

// We will only check correctness of the results here as checking the plan in JSTests is brittle and
// is better done in document_source_internal_unpack_bucket_test/group_reorder_test.cpp. For the
// cases when the re-write isn't applicable, the used datasets should yield wrong result if the
// re-write is applied.
export function runGroupRewriteTest(coll, docs, pipeline, expectedResults, excludeMeta) {
    coll.drop();
    if (excludeMeta) {
        db.createCollection(coll.getName(), {timeseries: {timeField: "time"}});
    } else {
        db.createCollection(coll.getName(), {timeseries: {metaField: "myMeta", timeField: "time"}});
    }
    coll.insertMany(docs);
    assert.sameMembers(expectedResults, coll.aggregate(pipeline).toArray(), () => {
        return `Pipeline: ${tojson(pipeline)}. Explain: ${
            tojson(coll.explain().aggregate(pipeline))}`;
    });
}

// Sometimes we just want to be sure that a rewrite applied and that the unpack stage was optimized
// away. Check in explain that the rewrite has happened.
export function assertUnpackOptimizedAway(coll, pipeline) {
    const explain = coll.explain().aggregate(pipeline);
    const unpack = (getEngine(explain) === "classic")
        ? getAggPlanStage(explain, "$_internalUnpackBucket")
        : getPlanStage(explain, "UNPACK_TS_BUCKET");
    // The rewrite should remove the unpack stage and replace it with a $group over the buckets
    // collection.
    assert(!unpack,
           `Expected to find no unpack stage for pipeline ${tojson(pipeline)} but got ${
               tojson(explain)}`);
}
