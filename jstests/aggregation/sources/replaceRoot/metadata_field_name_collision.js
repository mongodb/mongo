/**
 * Tests that user-produced documents with $-prefixed field names matching internal metadata field
 * names (e.g. $textScore, $pt, $changeStreamControlEvent) survive as regular document fields and
 * are not consumed as internal metadata by DocumentStorage::loadLazyMetadata().
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   do_not_wrap_aggregations_in_facets,
 *   does_not_support_transactions,
 * ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {describe, it, before} from "jstests/libs/mochalite.js";

/**
 * Asserts that a document with {fieldName: value} round-trips through $literal + $replaceWith
 * without the field being consumed as metadata.
 */
function assertLiteralRoundTrip(fieldName, value) {
    const doc = {[fieldName]: value};
    const actual = db.aggregate([{$documents: [{}]}, {$replaceWith: {$literal: doc}}]).toArray();
    assertArrayEq({actual, expected: [doc]});
}

/**
 * Asserts that setting fieldName via $setField and reading it back via $getField returns the
 * original value, verifying the field is not hijacked as metadata mid-pipeline.
 */
function assertSetGetRoundTrip(fieldName) {
    const result = db[jsTestName()]
        .aggregate([
            {$match: {_id: 0}},
            {
                $replaceWith: {
                    $setField: {field: {$const: fieldName}, input: "$$ROOT", value: "injected"},
                },
            },
            {$project: {_id: 0, retrieved: {$getField: {$const: fieldName}}}},
        ])
        .toArray();
    assertArrayEq({actual: result, expected: [{retrieved: "injected"}]});
}

// All 17 internal metadata field names from document.h:117-133.
const kMetadataFieldNames = [
    "$textScore",
    "$searchScore",
    "$randVal",
    "$dis",
    "$score",
    "$vectorSearchScore",
    "$pt",
    "$sortKey",
    "$indexKey",
    "$searchScoreDetails",
    "$searchSortValues",
    "$searchHighlights",
    "$searchSequenceToken",
    "$scoreDetails",
    "$stream",
    "$changeStreamControlEvent",
];

const kTestValues = ["str", 42, true, [1, 2], {a: 1}];

describe("metadata field name collision", () => {
    before(() => {
        const coll = assertDropAndRecreateCollection(db, jsTestName());
        assert.commandWorked(coll.insert({_id: 0, x: 1}));
    });

    // field x value cartesian product via $literal.
    for (const fieldName of kMetadataFieldNames) {
        for (const value of kTestValues) {
            it(`${fieldName} with ${tojson(value)} survives via $literal`, () => {
                assertLiteralRoundTrip(fieldName, value);
            });
        }
    }

    // field x $setField/$getField round-trip.
    for (const fieldName of kMetadataFieldNames) {
        it(`$setField/$getField round-trip for ${fieldName}`, () => {
            assertSetGetRoundTrip(fieldName);
        });
    }

    // All metadata fields in a single document.
    it("all metadata fields survive together in one document", () => {
        const doc = Object.fromEntries(kMetadataFieldNames.map((f, i) => [f, i]));
        doc.regular = "e";
        const actual = db.aggregate([{$documents: [{}]}, {$replaceWith: {$literal: doc}}]).toArray();
        assertArrayEq({actual, expected: [doc]});
    });
});
