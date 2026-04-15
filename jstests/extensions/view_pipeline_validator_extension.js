/**
 * Tests that the view_pipeline_validator extension ($validateViewPipeline stage) correctly
 * validates view pipelines when aggregating on a view: it accepts only $match, $addFields, and
 * $set stages in the view definition and rejects any other stage. The valid-pipeline helper also
 * asserts that each result document has storedViewPipelineLength matching the view pipeline
 * length, verifying the extension stores the pipeline and references it later (ownership/lifetime).
 * The stage serializes the view pipeline in its BSON so shards receive it when mongos sends the
 * pipeline; thus every document (including from shards and merge) has the field.
 *
 * @tags: [featureFlagExtensionsAPI, featureFlagExtensionsInsideHybridSearch]
 */

import {before, describe, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();
const rejectErrorCode = 11905602;

/**
 * Creates a view, runs $validateViewPipeline, asserts success, expected document count,
 * storedViewPipelineLength, and viewPipelineStages (array of stage names). Drops the view when done.
 */
function testValidViewPipeline(suffix, viewPipeline, expectedCount) {
    const viewName = collName + suffix;
    assert.commandWorked(db.createView(viewName, collName, viewPipeline));
    try {
        const docs = db
            .getCollection(viewName)
            .aggregate([{$validateViewPipeline: {}}])
            .toArray();
        assert.eq(expectedCount, docs.length, docs);
        const expectedStageNames = viewPipeline.map((stage) => Object.keys(stage)[0]);
        for (const doc of docs) {
            assert.eq(
                doc.storedViewPipelineLength,
                viewPipeline.length,
                "each doc should expose stored view pipeline length",
            );
            assert.eq(
                doc.viewPipelineStages,
                expectedStageNames,
                "each doc should expose viewPipelineStages (stage names)",
            );
        }
    } finally {
        assert.commandWorked(db.runCommand({drop: viewName}));
    }
}

/**
 * Creates a view, runs $validateViewPipeline, asserts command failure with rejectErrorCode,
 * then drops the view. Use for invalid view pipeline tests.
 */
function testInvalidViewPipeline(suffix, viewPipeline) {
    const viewName = collName + suffix;
    assert.commandWorked(db.createView(viewName, collName, viewPipeline));
    try {
        const res = db.runCommand({
            aggregate: viewName,
            pipeline: [{$validateViewPipeline: {}}],
            cursor: {},
        });
        assert.commandFailedWithCode(res, rejectErrorCode, res);
    } finally {
        assert.commandWorked(db.runCommand({drop: viewName}));
    }
}

describe("View pipeline validator extension", function () {
    let coll;

    before(function () {
        coll = db[collName];
        coll.drop();
        assert.commandWorked(
            coll.insertMany([
                {_id: 1, x: 10},
                {_id: 2, x: 20},
                {_id: 3, x: 30},
            ]),
        );
    });

    describe("Valid view pipelines", function () {
        it("should accept view with only $match", function () {
            testValidViewPipeline("_valid_match", [{$match: {x: {$gte: 15}}}], 2);
        });

        it("should accept view with only $addFields", function () {
            testValidViewPipeline("_valid_addFields", [{$addFields: {y: 1}}], 3);
        });

        it("should accept view with only $set", function () {
            testValidViewPipeline("_valid_set", [{$set: {z: 2}}], 3);
        });

        it("should accept view with $match and $addFields", function () {
            testValidViewPipeline("_valid_mixed", [{$match: {x: 20}}, {$addFields: {a: 1}}], 1);
        });

        it("should accept empty view pipeline", function () {
            testValidViewPipeline("_valid_empty", [], 3);
        });
    });

    describe("Invalid view pipelines", function () {
        it("should reject view with $project", function () {
            testInvalidViewPipeline("_invalid_project", [{$project: {x: 1, _id: 0}}]);
        });

        it("should reject view with $lookup", function () {
            testInvalidViewPipeline("_invalid_lookup", [
                {$match: {}},
                {$set: {z: 2}},
                {$lookup: {from: collName, localField: "_id", foreignField: "_id", as: "joined"}},
            ]);
        });
    });
});
