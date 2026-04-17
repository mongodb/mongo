/**
 * Verifies that indexes on timeseries collections are used inside $lookup subpipelines.
 *
 * Regression test for SERVER-95352, where the 'assumeNoMixedSchemaData' flag was not propagated to
 * the $_internalUnpackBucket stage inside subpipelines, causing an un-indexable type-equality
 * predicate to be generated and forcing a collection scan.
 *
 * @tags: [
 *   requires_timeseries,
 *   references_foreign_collection,
 *   requires_pipeline_optimization,
 *   # TODO SERVER-88275: background moveCollections can cause aggregation to fail with
 *   # QueryPlanKilled.
 *   assumes_balancer_off,
 * ]
 */
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {describe, it, before} from "jstests/libs/mochalite.js";

const testDB = db.getSiblingDB(jsTestName());
const timeField = "t";
const metaField = "m";
const now = ISODate();

function assertLookupUsesIndex(coll, pipeline, expectedResults) {
    const results = coll.aggregate(pipeline).toArray();
    assert.eq(expectedResults.count, results.length, tojson(results));
    if (expectedResults.validate) {
        expectedResults.validate(results);
    }

    const explain = coll.explain("executionStats").aggregate(pipeline);
    const lookupStages = getAggPlanStages(explain, "$lookup");

    if (lookupStages.length == 0) {
        assert(
            FixtureHelpers.isMongos(coll.getDB()),
            "Expected $lookup stage in non-sharded explain output: " + tojson(explain),
        );
        // In sharded environments where the local and foreign collections are on different
        // shards, the $lookup ends up in splitPipeline.mergerPart. Execution stats
        // (collectionScans, indexesUsed) are not available for merger pipeline stages since
        // they aren't propagated across the wire. Verify the $lookup is present and return.
        if (explain.splitPipeline) {
            const mergerPart = explain.splitPipeline.mergerPart || [];
            assert(
                mergerPart.some((stage) => stage.hasOwnProperty("$lookup")),
                "Expected $lookup in splitPipeline.mergerPart: " + tojson(explain),
            );
            return;
        }
        assert(false, "Expected $lookup stage in explain output: " + tojson(explain));
    }

    for (const stage of lookupStages) {
        if (
            FixtureHelpers.isMongos(coll.getDB()) &&
            stage.totalDocsExamined == 0 &&
            stage.totalKeysExamined == 0 &&
            stage.indexesUsed.length == 0
        ) {
            // When the foreign collection is on a different shard, execution stats
            // (collectionScans, indexesUsed) are not propagated back, so all counters read
            // zero. We cannot validate index usage in this case.
            continue;
        }
        assert.eq(stage.collectionScans, 0, "Expected no collection scans in $lookup: " + tojson(stage));
        assert.gt(stage.indexesUsed.length, 0, "Expected index use in $lookup: " + tojson(stage));
    }
}

describe("$lookup index use on timeseries foreign collection", function () {
    before(function () {
        testDB.dropDatabase();

        this.localColl = testDB.getCollection("local");
        this.foreignColl = testDB.getCollection("foreign_ts");

        assert.commandWorked(
            testDB.createCollection(this.foreignColl.getName(), {
                timeseries: {timeField: timeField, metaField: metaField},
            }),
        );
        assert.commandWorked(this.foreignColl.createIndex({a: 1}));

        const docs = [];
        for (let i = 0; i < 100; i++) {
            docs.push({
                [timeField]: new Date(now.getTime() + i * 1000),
                [metaField]: i % 10,
                a: i,
                b: i * 2,
            });
        }
        assert.commandWorked(this.foreignColl.insertMany(docs));
        assert.commandWorked(
            this.localColl.insertMany([
                {_id: 0, key: 5},
                {_id: 1, key: 50},
            ]),
        );
    });

    it("localField/foreignField join uses index", function () {
        const pipeline = [
            {
                $lookup: {
                    from: this.foreignColl.getName(),
                    localField: "key",
                    foreignField: "a",
                    as: "matched",
                },
            },
        ];

        assertLookupUsesIndex(this.localColl, pipeline, {
            count: 2,
            validate: (results) => {
                for (const result of results) {
                    assert.eq(1, result.matched.length, tojson(result));
                    assert.eq(
                        result.key,
                        result.matched[0].a,
                        "Expected matched 'a' to equal localField 'key': " + tojson(result),
                    );
                }
            },
        });
    });

    it("subpipeline equality $match uses index", function () {
        const pipeline = [
            {
                $lookup: {
                    from: this.foreignColl.getName(),
                    pipeline: [{$match: {a: 5}}],
                    as: "matched",
                },
            },
        ];

        assertLookupUsesIndex(this.localColl, pipeline, {
            count: 2,
            validate: (results) => {
                for (const result of results) {
                    assert.eq(1, result.matched.length, tojson(result));
                    assert.eq(5, result.matched[0].a, tojson(result));
                }
            },
        });
    });
});
