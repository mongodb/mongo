/**
 * Test collation behavior for $merge stage.
 * @tags: [
 *  assumes_unsharded_collection,
 *  requires_fcv_83
 * ]
 */

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const caseInsensitiveCollation = {locale: "en", strength: 1};

db.dropDatabase();
const outputColl = db[jsTestName() + "_merge_output"];
assert.commandWorked(outputColl.createIndex({unique: 1}, {unique: true, collation: caseInsensitiveCollation}));

function resetCollection() {
    assert.commandWorked(outputColl.deleteMany({}));
    assert.commandWorked(outputColl.insertOne({unique: "abc", data: "original"}));
}

function runReplaceInsertTest() {
    resetCollection();

    db.aggregate(
        [
            {
                $documents: [
                    {unique: "ABC", data: "replaced"}, // Should match "abc" due to collation
                    {unique: "def", data: "new"}, // Should not match
                ],
            },
            {
                $merge: {
                    into: outputColl.getName(),
                    on: ["unique"],
                    whenMatched: "replace",
                    whenNotMatched: "insert",
                },
            },
        ],
        {collation: caseInsensitiveCollation},
    );

    const result = outputColl.find({}).collation(caseInsensitiveCollation).toArray();
    assert.eq(2, result.length, "Should have 2 documents after replace");
    assert.eq("replaced", result.find((doc) => doc.unique === "ABC").data);
    assert.eq("new", result.find((doc) => doc.unique === "def").data);
}

function runMergeInsertTest() {
    resetCollection();

    db.aggregate(
        [
            {
                $documents: [
                    {unique: "ABC", newField: "merged"},
                    {unique: "xyz", data: "new2"},
                ],
            },
            {
                $merge: {
                    into: outputColl.getName(),
                    on: ["unique"],
                    whenMatched: "merge",
                    whenNotMatched: "insert",
                },
            },
        ],
        {collation: caseInsensitiveCollation},
    );

    const result = outputColl.find({}).collation(caseInsensitiveCollation).toArray();
    assert.eq(2, result.length, "Should have 2 documents after merge");
    const mergedDoc = result.find((doc) => doc.unique.toLowerCase() === "abc");
    assert.eq("original", mergedDoc.data, "Original data should be preserved");
    assert.eq("merged", mergedDoc.newField, "New field should be added");
}

function runPipelineInsertTest() {
    resetCollection();

    db.aggregate(
        [
            {
                $documents: [
                    {unique: "ABC", increment: 1},
                    {unique: "ghi", data: "new3"},
                ],
            },
            {
                $merge: {
                    into: outputColl.getName(),
                    on: ["unique"],
                    whenMatched: [{$set: {data: {$concat: ["$data", "_updated"]}}}],
                    whenNotMatched: "insert",
                },
            },
        ],
        {collation: caseInsensitiveCollation},
    );

    const result = outputColl.find({}).collation(caseInsensitiveCollation).toArray();
    assert.eq(2, result.length, "Should have 2 documents after pipeline");
    assert.eq("original_updated", result.find((doc) => doc.unique.toLowerCase() === "abc").data);
}

function runFailInsertTest() {
    resetCollection();

    assertErrorCode(
        db,
        [
            {$documents: [{unique: "ABC", data: "should_fail"}]},
            {
                $merge: {
                    into: outputColl.getName(),
                    on: ["unique"],
                    whenMatched: "fail",
                    whenNotMatched: "insert",
                },
            },
        ],
        ErrorCodes.DuplicateKey,
        "Should fail when matched",
        {collation: caseInsensitiveCollation},
    );

    db.aggregate(
        [
            {$documents: [{unique: "ghi", data: "new3"}]},
            {
                $merge: {
                    into: outputColl.getName(),
                    on: ["unique"],
                    whenMatched: "fail",
                    whenNotMatched: "insert",
                },
            },
        ],
        {collation: caseInsensitiveCollation},
    );

    const result = outputColl.find({}).collation(caseInsensitiveCollation).toArray();
    assert.eq(2, result.length, "Should have 2 documents after merge");
    assert.eq("original", result.find((doc) => doc.unique === "abc").data);
    assert.eq("new3", result.find((doc) => doc.unique === "ghi").data);
}

function runKeepExistingInsertTest() {
    resetCollection();

    db.aggregate(
        [
            {
                $documents: [
                    {unique: "ABC", data: "discarded"},
                    {unique: "jkl", data: "new4"},
                ],
            },
            {
                $merge: {
                    into: outputColl.getName(),
                    on: ["unique"],
                    whenMatched: "keepExisting",
                    whenNotMatched: "insert",
                },
            },
        ],
        {collation: caseInsensitiveCollation},
    );

    const result = outputColl.find({}).collation(caseInsensitiveCollation).toArray();
    assert.eq(2, result.length, "Should have 2 documents after discard");
    assert.eq(
        "original",
        result.find((doc) => doc.unique.toLowerCase() === "abc").data,
        "Original should be unchanged",
    );
    assert.eq("new4", result.find((doc) => doc.unique === "jkl").data);
}

function runReplaceDiscardTest() {
    resetCollection();

    db.aggregate(
        [
            {
                $documents: [
                    {unique: "ABC", data: "updated"},
                    {unique: "mno", data: "discarded"},
                ],
            },
            {
                $merge: {
                    into: outputColl.getName(),
                    on: ["unique"],
                    whenMatched: "replace",
                    whenNotMatched: "discard",
                },
            },
        ],
        {collation: caseInsensitiveCollation},
    );

    const result = outputColl.find({}).collation(caseInsensitiveCollation).toArray();
    assert.eq(1, result.length, "Should have 1 document after discard non-match");
    assert.eq("updated", result[0].data);
}

function runReplaceFailTest() {
    resetCollection();

    assertErrorCode(
        db,
        [
            {$documents: [{unique: "pqr", data: "should_fail"}]}, // Should not match and fail
            {
                $merge: {
                    into: outputColl.getName(),
                    on: ["unique"],
                    whenMatched: "replace",
                    whenNotMatched: "fail",
                },
            },
        ],
        ErrorCodes.MergeStageNoMatchingDocument,
        "Should fail when not matched",
        {collation: caseInsensitiveCollation},
    );

    db.aggregate(
        [
            {$documents: [{unique: "ABC", data: "replaced"}]},
            {
                $merge: {
                    into: outputColl.getName(),
                    on: ["unique"],
                    whenMatched: "replace",
                    whenNotMatched: "fail",
                },
            },
        ],
        {collation: caseInsensitiveCollation},
    );

    const result = outputColl.find({}).collation(caseInsensitiveCollation).toArray();
    assert.eq(1, result.length, "Should have 1 document after discard non-match");
    assert.eq("replaced", result[0].data);
}

runReplaceInsertTest();
runMergeInsertTest();
runPipelineInsertTest();
runFailInsertTest();
runKeepExistingInsertTest();
runReplaceDiscardTest();
runReplaceFailTest();
