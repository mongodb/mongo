/**
 * Helper for testing the behavior of $group on time-series collections.
 */

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
