/**
 * Tests that extension stages are rejected with non-local read concern queries.
 *
 * Verifies that:
 * 1. Extension stages work with local read concern (the default)
 * 2. Extension stages fail with InvalidOptions when used with non-local read concerns (snapshot,
 *    majority)
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();

describe("Extension stage read concern restrictions", function () {
    const extensionPipeline = [{$testFoo: {}}];
    let testDB;
    let testColl;

    before(function () {
        testDB = db.getSiblingDB(jsTestName());
        testColl = testDB[collName];

        // Set up the test collection.
        testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
        assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));
        assert.commandWorked(
            testColl.insertMany(
                [
                    {_id: 0, x: 1},
                    {_id: 1, x: 2},
                    {_id: 2, x: 3},
                ],
                {writeConcern: {w: "majority"}},
            ),
        );
    });

    after(function () {
        testDB.runCommand({drop: collName});
    });

    it("should succeed with default read concern", function () {
        const result = assert.commandWorked(
            testDB.runCommand({aggregate: collName, pipeline: extensionPipeline, cursor: {}}),
        );
        assert.eq(result.cursor.firstBatch.length, 3, "Expected 3 documents with default read concern");
    });

    it("should succeed with explicit local read concern", function () {
        const cmd = {
            aggregate: collName,
            pipeline: extensionPipeline,
            cursor: {},
            readConcern: {level: "local"},
        };
        const result = assert.commandWorked(testDB.runCommand(cmd));
        assert.eq(result.cursor.firstBatch.length, 3, "Expected 3 documents with local read concern");
    });

    it("should fail with snapshot read concern", function () {
        const cmd = {
            aggregate: collName,
            pipeline: extensionPipeline,
            cursor: {},
            readConcern: {level: "snapshot"},
        };
        assert.commandFailedWithCode(
            testDB.runCommand(cmd),
            ErrorCodes.InvalidOptions,
            "Extension stage should be rejected with readConcern: snapshot",
        );
    });

    it("should fail with majority read concern", function () {
        const cmd = {
            aggregate: collName,
            pipeline: extensionPipeline,
            cursor: {},
            readConcern: {level: "majority"},
        };
        assert.commandFailedWithCode(
            testDB.runCommand(cmd),
            ErrorCodes.InvalidOptions,
            "Extension stage should be rejected with readConcern: majority",
        );
    });
});
