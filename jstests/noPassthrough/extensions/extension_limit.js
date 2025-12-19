/**
 * Correctness tests for the extension $extensionLimit stage.
 * Verifies that $extensionLimit produces identical results to the native $limit stage
 * across various pipeline configurations and edge cases.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {checkPlatformCompatibleWithExtensions, withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

// Helper to verify native $limit and extension $extensionLimit produce identical results.
function assertLimitEquivalence(
    nativePipeline,
    extensionPipeline,
    testName,
    collection,
    expectExtensionLimitInShardsPart = true,
) {
    const nativeResults = collection.aggregate(nativePipeline).toArray();
    const extensionResults = collection.aggregate(extensionPipeline).toArray();
    if (!FixtureHelpers.isSharded(collection)) {
        // The results for native $limit and $extensionLimit in single node, single shard, and
        // sharded cluster topologies are expected to be deterministic.
        assert.sameMembers(
            extensionResults,
            nativeResults,
            `${testName}: Expect $extensionLimit to produce same results as native $limit in an unsharded topology`,
        );
    } else {
        // The native $limit and $extensionLimit are non-deterministic so only the num docs of
        // returned, instead of the result sets, should be compared.
        assert.eq(
            extensionResults.length,
            nativeResults.length,
            `${testName}: Extension $extensionLimit must produce same number of results as native $limit`,
        );
        // For the sharded collections topology, the execution stats for $extensionLimit was
        // also examined to confirm that the pipeline gets split and the stage runs in both the
        // merging and shards pipeline.
        const extensionExplain = collection.explain("executionStats").aggregate(extensionPipeline);

        assert(
            extensionExplain.hasOwnProperty("shards"),
            "Extension limit explain should have shards in sharded topology",
        );

        const shards = extensionExplain.shards;

        if (Object.keys(shards).length > 1) {
            assert(
                extensionExplain.hasOwnProperty("splitPipeline"),
                "Extension limit explain should have splitPipeline in sharded topology",
            );

            // Assert shardsPart has $extensionLimit.
            if (expectExtensionLimitInShardsPart) {
                assert(
                    extensionExplain.splitPipeline.hasOwnProperty("shardsPart"),
                    "splitPipeline should have shardsPart",
                );
                const shardsLimitStage = extensionExplain.splitPipeline.shardsPart.find((stage) =>
                    stage.hasOwnProperty("$extensionLimit"),
                );
                assert(shardsLimitStage, "shardsPart should contain $extensionLimit stage");
            }

            // Assert mergerPart has $extensionLimit.
            assert(extensionExplain.splitPipeline.hasOwnProperty("mergerPart"), "splitPipeline should have mergerPart");
            const mergerLimitStage = extensionExplain.splitPipeline.mergerPart.find((stage) =>
                stage.hasOwnProperty("$extensionLimit"),
            );
            assert(mergerLimitStage, "mergerPart should contain $extensionLimit stage");
        }
    }
    return extensionResults.length;
}

const numDocuments = 1000;

// ===========================
// Setup test data
// ===========================
function setupCollection(conn, db, coll) {
    if (conn.isMongos()) {
        // If we’re on mongos, set up sharding for this collection.
        const res = db.adminCommand({listShards: 1});
        assert.commandWorked(res);
        const shardIds = res.shards.map((s) => s._id);
        assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
        assert.commandWorked(coll.createIndex({x: 1}));
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));
        assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {x: Math.floor(numDocuments / 2)}}));

        assert(shardIds.length > 1);
        assert.commandWorked(db.adminCommand({moveChunk: coll.getFullName(), find: {x: 250}, to: shardIds[0]}));
        assert.commandWorked(db.adminCommand({moveChunk: coll.getFullName(), find: {x: 750}, to: shardIds[1]}));
    }

    const bulkDocs = [];
    const categories = ["A", "B", "C"];
    for (let i = 0; i < numDocuments; i++) {
        bulkDocs.push({_id: i, x: i, value: i * 2, category: categories[i % categories.length]});
    }
    coll.insertMany(bulkDocs);
}

function runTests(conn) {
    const testDB = conn.getDB(jsTestName());
    const collName = jsTestName();
    const coll = testDB[collName];
    coll.drop();
    setupCollection(conn, testDB, coll);
    try {
        // ===========================
        // Basic limit tests
        // ===========================
        // Test 1: Basic limit
        assertLimitEquivalence([{$limit: 5}], [{$extensionLimit: 5}], "Basic limit (5 docs from 10)", coll);

        // Test 2: Limit of 1 (minimal case)
        assertLimitEquivalence([{$limit: 1}], [{$extensionLimit: 1}], "Limit 1 (minimal limit)", coll);

        // Test 3: Limit larger than collection
        assertLimitEquivalence(
            [{$limit: numDocuments + 1}],
            [{$extensionLimit: numDocuments + 1}],
            "Limit larger than collection",
            coll,
        );

        // Test 4: Limit exactly matching collection size
        assertLimitEquivalence(
            [{$limit: numDocuments}],
            [{$extensionLimit: numDocuments}],
            "Limit equals collection size",
            coll,
        );

        // ===========================
        // Limit with other stages
        // ===========================

        // Test 5: Limit with preceding $match
        assertLimitEquivalence(
            [{$match: {category: "A"}}, {$limit: 2}],
            [{$match: {category: "A"}}, {$extensionLimit: 2}],
            "Match then limit",
            coll,
        );

        // Test 6: Limit with following $project
        const projected = assertLimitEquivalence(
            [{$limit: 3}, {$project: {x: 1, _id: 0}}],
            [{$extensionLimit: 3}, {$project: {x: 1, _id: 0}}],
            "Limit then project",
            coll,
        );
        assert.eq(projected, 3, "Should return 3 documents");

        // Test 7: Limit with preceding $sort.
        // Note that the $extensionLimit does not benefit from the top-N optimization so it is
        // removed from the shards pipeline.
        const sorted = coll.aggregate([{$sort: {value: -1}}, {$extensionLimit: 3}]).toArray();
        assert.eq(sorted.length, 3, "Should return 3 documents");
        assert.gte(sorted[0].value, sorted[1].value, "First doc should have highest value");
        assert.gte(sorted[1].value, sorted[2].value, "Second doc should have second highest value");

        assertLimitEquivalence(
            [{$sort: {value: -1}}, {$limit: 3}],
            [{$sort: {value: -1}}, {$extensionLimit: 3}],
            "Sort then limit (top-N)",
            coll,
            false,
        );

        // Test 8: Complex pipeline with limit in middle.
        // Note that the $extensionLimit does not benefit from the top-N optimization so it is
        // removed from the shards pipeline.
        assertLimitEquivalence(
            [{$match: {x: {$gte: 3}}}, {$sort: {x: 1}}, {$limit: 4}, {$project: {x: 1}}],
            [
                {$match: {x: {$gte: 3}}},
                {$sort: {x: 1}},
                {$extensionLimit: 4},
                {
                    $project: {
                        x: 1,
                    },
                },
            ],
            "Complex pipeline with limit in middle",
            coll,
            false,
        );

        // Test 9: Multiple limits in sequence (smaller wins)
        assertLimitEquivalence(
            [{$limit: 8}, {$limit: 3}],
            [{$extensionLimit: 8}, {$extensionLimit: 3}],
            "Multiple limits - smaller wins",
            coll,
        );

        assertLimitEquivalence(
            [{$limit: 2}, {$limit: 10}],
            [{$extensionLimit: 2}, {$extensionLimit: 10}],
            "Multiple limits - first is smaller",
            coll,
        );

        // Test 10: Limit with $skip
        assertLimitEquivalence(
            [{$skip: 3}, {$limit: 4}],
            [{$skip: 3}, {$extensionLimit: 4}],
            "Skip then limit",
            coll,
            false,
        );

        assertLimitEquivalence([{$limit: 7}, {$skip: 2}], [{$extensionLimit: 7}, {$skip: 2}], "Limit then skip", coll);

        // Test 11: Limit after $group.
        // $group is split before $extensionLimit, nothing should get pushed to shards (don't limit
        // on partial results). Even if $limit was pushed to the shards after $group,
        // $extensionLimit wouldn't be pushed since it's not recognized.
        assertLimitEquivalence(
            [{$group: {_id: "$category", total: {$sum: "$value"}}}, {$sort: {_id: 1}}, {$limit: 2}],
            [{$group: {_id: "$category", total: {$sum: "$value"}}}, {$sort: {_id: 1}}, {$extensionLimit: 2}],
            "Group then limit",
            coll,
            false,
        );

        // Test 12: Limit with $unwind
        coll.insert({_id: 100, arr: [1, 2, 3, 4, 5], temp: true});
        assertLimitEquivalence(
            [{$match: {temp: true}}, {$unwind: "$arr"}, {$limit: 3}],
            [{$match: {temp: true}}, {$unwind: "$arr"}, {$extensionLimit: 3}],
            "Unwind then limit",
            coll,
        );
        coll.deleteOne({_id: 100});

        // Test 13: Limit with $addFields
        assertLimitEquivalence(
            [{$limit: 5}, {$addFields: {computed: {$multiply: ["$x", 2]}}}],
            [{$extensionLimit: 5}, {$addFields: {computed: {$multiply: ["$x", 2]}}}],
            "Limit then addFields",
            coll,
        );

        // ===========================
        // Edge cases
        // ===========================

        // Test 14: Limit on empty result set
        assertLimitEquivalence(
            [{$match: {category: "Z"}}, {$limit: 5}],
            [{$match: {category: "Z"}}, {$extensionLimit: 5}],
            "Limit on empty result set",
            coll,
        );

        // Test 15: Very large limit value (near max long long)
        assertLimitEquivalence(
            [{$limit: NumberLong("9223372036854775807")}],
            [{$extensionLimit: NumberLong("9223372036854775807")}],
            "Very large limit value (max long)",
            coll,
        );

        // Test 16: Limit with batched cursor
        const batchedNative = [];
        const batchedExtension = [];
        let cursorNative = coll.aggregate([{$limit: 7}], {cursor: {batchSize: 2}});
        let cursorExtension = coll.aggregate([{$extensionLimit: 7}], {cursor: {batchSize: 2}});

        while (cursorNative.hasNext()) {
            batchedNative.push(cursorNative.next());
        }
        while (cursorExtension.hasNext()) {
            batchedExtension.push(cursorExtension.next());
        }
        if (!FixtureHelpers.isSharded(coll)) {
            assert.eq(batchedExtension, batchedNative, "Batched cursor results should match");
        } else {
            assert.eq(batchedExtension.length, batchedNative.length, "Batched cursor result lengths should match");
        }
        // Test 17: Top-N query with sort + limit
        const topN = coll.aggregate([{$sort: {value: -1}}, {$extensionLimit: 5}]).toArray();
        assert.eq(topN.length, 5, "Should return 5 documents");
        assert.eq(topN[0].value, 1998, "Top value should be highest");

        assertLimitEquivalence(
            [{$sort: {value: -1}}, {$limit: 5}],
            [{$sort: {value: -1}}, {$extensionLimit: 5}],
            "Top-N with sort + limit",
            coll,
            false,
        );

        // Test 18: match + limit
        assertLimitEquivalence(
            [{$match: {category: 5}}, {$limit: 20}],
            [{$match: {category: 5}}, {$extensionLimit: 20}],
            "Indexed match then limit",
            coll,
        );

        // Test 19: match + sort + limit
        assertLimitEquivalence(
            [{$match: {category: 3}}, {$sort: {value: 1}}, {$limit: 10}],
            [{$match: {category: 3}}, {$sort: {value: 1}}, {$extensionLimit: 10}],
            "Indexed match + sort + limit",
            coll,
            false,
        );

        // ===========================
        // Error cases
        // ===========================

        // Both should fail with limit 0
        assert.commandFailedWithCode(
            testDB.runCommand({aggregate: collName, pipeline: [{$limit: 0}], cursor: {}}),
            15958,
            "Native $limit with 0 should fail",
        );

        assert.commandFailedWithCode(
            testDB.runCommand({aggregate: collName, pipeline: [{$extensionLimit: 0}], cursor: {}}),
            11484702,
            "Extension $extensionLimit with 0 should fail",
        );

        // Both should fail with negative limit
        assert.commandFailedWithCode(
            testDB.runCommand({aggregate: collName, pipeline: [{$limit: -5}], cursor: {}}),
            5107201,
            "Native $limit with negative value should fail",
        );

        assert.commandFailedWithCode(
            testDB.runCommand({aggregate: collName, pipeline: [{$extensionLimit: -5}], cursor: {}}),
            11484701,
            "Extension $extensionLimit with negative value should fail",
        );

        // Both should fail with non-numeric limit
        assert.commandFailedWithCode(
            testDB.runCommand({aggregate: collName, pipeline: [{$limit: "abc"}], cursor: {}}),
            5107201,
            "Native $limit with string should fail",
        );

        assert.commandFailedWithCode(
            testDB.runCommand({aggregate: collName, pipeline: [{$extensionLimit: "abc"}], cursor: {}}),
            11484701,
            "Extension $extensionLimit with string should fail",
        );

        // ===========================
        // Subpipeline tests (extension not supported yet)
        // ===========================
        // const otherColl = testDB[collName + "_other"];

        // otherColl.drop();
        // otherColl.insertMany([
        //     {_id: 0, y: 10},
        //     {_id: 1, y: 20},
        //     {_id: 2, y: 30},
        // ]);

        // TODO SERVER-115071: Extension $extensionLimit cannot be run in subpipelines yet due to
        // missing constraint overrides (FacetRequirement, LookupRequirement, UnionRequirement).
        // Uncomment these correctness tests once the constraints are updated.

        // print("\n=== Subpipeline correctness tests ===");

        // Test 20: Limit in $lookup subpipeline
        // assertLimitEquivalence(
        //     [{$limit: 2}, {$lookup: {from: otherColl.getName(), as: "joined", pipeline: [{$limit:
        //     2}]}}],
        //     [{$limit: 2}, {$lookup: {from: otherColl.getName(), as: "joined", pipeline:
        //     [{$extensionLimit: 2}]}}], "Test 21: Limit in $lookup subpipeline", coll
        // );

        // Test 21: Limit in $unionWith subpipeline
        // assertLimitEquivalence(
        //     [{$limit: 3}, {$unionWith: {coll: otherColl.getName(), pipeline: [{$limit: 1}]}}],
        //     [{$limit: 3}, {$unionWith: {coll: otherColl.getName(), pipeline: [{$extensionLimit:
        //     1}]}}], "Test 22: Limit in $unionWith subpipeline", coll
        // );

        // Test 22: Limit in $facet subpipeline
        // assertLimitEquivalence(
        //     [{$facet: {
        //         limited: [{$limit: 3}],
        //         sorted: [{$sort: {x: -1}}, {$limit: 2}]
        //     }}],
        //     [{$facet: {
        //         limited: [{$extensionLimit: 3}],
        //         sorted: [{$sort: {x: -1}}, {$extensionLimit: 2}]
        //     }}],
        //     "Test 23: Limit in $facet subpipeline", coll
        // );

        print("\n✓ Subpipeline tests skipped until SERVER-115071 is resolved");

        // ===========================
        // Transaction test (extension not supported yet)
        // ===========================

        // TODO SERVER-115071: Extension $extensionLimit cannot be run in transactions yet due to
        // missing constraint override (TransactionRequirement). Uncomment this correctness test
        // once the constraint is updated.

        // Test 23: Limit in transaction
        // const session = testDB.getMongo().startSession();
        // const sessionDb = session.getDatabase(testDB.getName());
        // const sessionColl = sessionDb[collName];
        //
        // session.startTransaction();
        // try {
        //     assertLimitEquivalence(
        //         [{$limit: 5}],
        //         [{$extensionLimit: 5}],
        //         "Test 24: Limit in transaction",
        //         sessionColl
        //     );
        //     session.commitTransaction();
        // } catch (e) {
        //     session.abortTransaction();
        //     throw e;
        // } finally {
        //     session.endSession();
        // }
        // otherColl.drop();

        print("\n✓ Transaction test skipped until SERVER-115071 is resolved");
    } finally {
        coll.drop();
    }
}

withExtensions({"liblimit_mongo_extension.so": {}}, runTests, ["sharded", "standalone", "replica_set"], 2);
