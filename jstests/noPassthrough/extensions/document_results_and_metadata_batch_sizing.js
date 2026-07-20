/**
 * Verifies $_internalDocumentResultsAndMetadata (DRM) forwards downstream docs-needed bounds
 * to its wrapped extension source.
 *
 * Uses $extensionMultiStream with reportObservedBounds:true, which tags each emitted document with
 * observedPipelineLimit, the docs-needed max bound the source observed. Three distinct values, so a
 * test can tell "the rule never ran" (e.g. broken dispatch) apart from "the rule ran and correctly
 * found no discrete bound":
 *   -1  the applyPipelineBounds rule never fired
 *   -2  the rule fired but found a non-discrete (Unknown/NeedAll) bound
 *    N  the rule fired and found the discrete bound N
 *
 * Covers both topologies:
 *  - Standalone: the pipeline is never split, so the source observes the outer suffix's $limit.
 *  - Sharded (single-shard cluster): with only one shard, mongos has nothing to merge and forwards
 *    the entire pipeline (including the downstream $limit) to that shard unmodified, so the
 *    shard-local DRM sees needsMerge=false and observes the bound exactly as in standalone.
 *
 * @tags: [
 *  featureFlagExtensionsAPI,
 *  requires_fcv_90,
 * ]
 */
import {
    checkPlatformCompatibleWithExtensions,
    withExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

withExtensions(
    {"libmulti_stream_mongo_extension.so": {}},
    (conn, shardingTest) => {
        const isSharded = shardingTest !== null;
        const testDB = conn.getDB("test");
        const coll = testDB[jsTestName()];
        assert.commandWorked(coll.insertOne({placeholder: true}));

        if (isSharded) {
            // Shard the collection so the query runs through mongos. With only one shard in this
            // fixture mongos still forwards the whole pipeline unmodified (see file comment above).
            assert.commandWorked(testDB.adminCommand({enableSharding: testDB.getName()}));
            assert.commandWorked(
                testDB.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}),
            );
        }

        // The source observes the downstream $limit and reports it as the discrete max bound in
        // both topologies: a single-shard cluster forwards the whole pipeline to its one shard
        // without a merge, so the shard-local view matches standalone exactly (see comment above).
        {
            const results = coll
                .aggregate([
                    {$extensionMultiStream: {numDocs: 10, reportObservedBounds: true}},
                    {$limit: 3},
                ])
                .toArray();
            assert.eq(results.length, 3, {results});
            for (const doc of results) {
                assert.eq(doc.observedPipelineLimit, 3, {doc, isSharded});
            }
        }

        // With no downstream limiting stage the max bound is Unknown (not discrete). -2 proves the
        // rule fired and correctly found a non-discrete bound, rather than never running.
        {
            const results = coll
                .aggregate([{$extensionMultiStream: {numDocs: 3, reportObservedBounds: true}}])
                .toArray();
            assert.eq(results.length, 3, {results});
            for (const doc of results) {
                assert.eq(doc.observedPipelineLimit, -2, {doc, isSharded});
            }
        }

        // A downstream blocking $sort forces NeedAll (not a discrete bound): expected error is -2.
        {
            const results = coll
                .aggregate([
                    {$extensionMultiStream: {numDocs: 3, reportObservedBounds: true}},
                    {$sort: {score: 1}},
                ])
                .toArray();
            for (const doc of results) {
                assert.eq(doc.observedPipelineLimit, -2, {doc, isSharded});
            }
        }
    },
    ["standalone", "sharded"],
    {},
    {setParameter: {featureFlagExtensionsOptimizations: true}},
);
