/**
 * Tests that metadata (sortKey) properly propagates through extension stages to downstream
 * stages that require it. This verifies the fix for metadata propagation when stages are pushed
 * down to the query executor.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();
const coll = db.getCollection(collName);

describe("extension stage metadata propagation", function () {
    before(function () {
        coll.drop();
        const docs = [];
        for (let i = 0; i < 10; i++) {
            docs.push({
                _id: i,
                value: i,
                text_field: i % 2 === 0 ? "test document" : "other document",
            });
        }
        assert.commandWorked(coll.insertMany(docs));
    });

    after(function () {
        coll.drop();
    });

    it("should preserve sortKey metadata through $vectorSearch for $setWindowFields", function () {
        // Verify $sort pushdown with $extensionLimit preserves sortKey metadata for
        // $setWindowFields. This tests that sortKey metadata flows through extension stages to
        // downstream stages that need it.
        const result = coll
            .aggregate([
                {$sort: {value: 1}},
                {$extensionLimit: 5},
                {
                    $_internalSetWindowFields: {
                        sortBy: {order: 1}, // This uses sortKey from the $sort
                        output: {rank: {$rank: {}}},
                    },
                },
                {$project: {_id: 1, value: 1, rank: 1}},
            ])
            .toArray();

        assert.eq(result.length, 5, "Expected 5 documents");
        // Verify ranking worked (requires sortKey metadata)
        for (let i = 0; i < result.length; i++) {
            assert.eq(result[i].rank, i + 1, `Document ${i} should have rank ${i + 1}`);
        }
    });

    it("should preserve textScore metadata through $extensionLimit to downstream stages", function () {
        // Test that score metadata (not pushed down) also flows correctly through extension stages
        // Create a text index for $text search
        assert.commandWorked(coll.createIndex({text_field: "text"}));

        const result = coll
            .aggregate([
                {$match: {$text: {$search: "test"}}}, // Generates textScore metadata
                {$extensionLimit: 3},
                {
                    $addFields: {
                        scoreValue: {$meta: "textScore"}, // Accesses score metadata
                    },
                },
                {$project: {_id: 1, text_field: 1, scoreValue: 1}},
            ])
            .toArray();

        // Verify that score metadata was accessible (should not throw error)
        result.forEach((doc) => {
            assert(doc.hasOwnProperty("scoreValue"), "Document should have scoreValue from textScore metadata");
            assert.gte(doc.scoreValue, 0, "Score should be non-negative");
        });

        assert.commandWorked(coll.dropIndexes());
    });

    it("should handle multiple extension stages in sequence with metadata", function () {
        // Test multiple extension stages with sortKey metadata flowing through all of them
        const result = coll
            .aggregate([
                {$sort: {value: 1}},
                {$extensionLimit: 8},
                {$extensionLimit: 5}, // Two extension stages in a row
                {
                    $_internalSetWindowFields: {
                        sortBy: {value: 1},
                        output: {rank: {$rank: {}}},
                    },
                },
                {$project: {_id: 1, value: 1, rank: 1}},
            ])
            .toArray();

        assert.eq(result.length, 5, "Expected 5 documents after two limits");
        // Verify ranking still works with multiple extension stages
        for (let i = 0; i < result.length; i++) {
            assert.eq(result[i].rank, i + 1, `Document ${i} should have rank ${i + 1}`);
        }
    });
});
