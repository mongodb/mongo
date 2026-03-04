/**
 * Tests that extension stages can modify their BSON representation in toBsonForLog() and that these
 * modifications appear correctly in slow query logs.
 *
 * This test verifies the $modifyForLog stage:
 * 1. Truncates large arrays in logs
 * 2. Summarizes nested objects with many fields in logs
 * 3. These modifications only affect logging, not the actual query execution
 *
 * @tags: [featureFlagExtensionsAPI]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();
const coll = db.getCollection(collName);

// Helper function to retrieve slow query logs for a command, given a comment.
function getSlowQueryLogsByComment(db, comment) {
    const slowQueryLogId = 51803; // ID for 'Slow query' log messages.

    let logs = checkLog.getFilteredLogMessages(db, slowQueryLogId, {command: {comment: comment}}, null, true) || [];
    // For sharded clusters, also check logs on all shards.
    if (FixtureHelpers.isMongos(db)) {
        const shardLogsArrays = FixtureHelpers.mapOnEachShardNode({
            db,
            primaryNodeOnly: true,
            func: (shardDb) =>
                checkLog.getFilteredLogMessages(shardDb, slowQueryLogId, {command: {comment: comment}}, null, true) ||
                [],
        });
        logs = logs.concat(...shardLogsArrays);
    }

    return logs;
}

describe("$modifyForLog toBsonForLog()", function () {
    before(function () {
        coll.drop();

        const testData = [
            {_id: 0, value: "data-1", category: "A"},
            {_id: 1, value: "data-2", category: "B"},
            {_id: 2, value: "data-3", category: "A"},
            {_id: 3, value: "data-4", category: "C"},
            {_id: 4, value: "data-5", category: "B"},
        ];
        assert.commandWorked(coll.insertMany(testData));

        // Set slowms to -1 to log all queries.
        // For sharded clusters, broadcast to all shards.
        assert.commandWorked(db.adminCommand({profile: 0, slowms: -1}));
        if (FixtureHelpers.isMongos(db)) {
            FixtureHelpers.runCommandOnEachPrimary({
                db: db.getSiblingDB("admin"),
                cmdObj: {profile: 0, slowms: -1},
            });
        }
    });

    after(function () {
        coll.drop();
    });

    beforeEach(function () {
        // Clear logs on mongos/mongod and all shards.
        assert.commandWorked(db.adminCommand({clearLog: "global"}));
        if (FixtureHelpers.isMongos(db)) {
            FixtureHelpers.runCommandOnEachPrimary({
                db: db.getSiblingDB("admin"),
                cmdObj: {clearLog: "global"},
            });
        }
    });

    it("should truncate large arrays in slow query logs", function () {
        const largeArray = [];
        for (let i = 0; i < 100; i++) {
            largeArray.push(i);
        }

        const comment = "truncate_large_arrays_test";
        const pipeline = [
            {
                $modifyForLog: {
                    largeArray: largeArray,
                    smallArray: [1, 2, 3],
                    normalField: "unchanged",
                },
            },
            {$limit: 2},
        ];

        const results = coll.aggregate(pipeline, {comment: comment}).toArray();
        assert.eq(results.length, 2, "Should return 2 documents");

        const logs = getSlowQueryLogsByComment(db, comment);
        assert.gte(logs.length, 1, "Should find at least one slow query log entry");

        const logEntry = logs[0];
        assert(logEntry.attr !== undefined, "Log entry should have 'attr' field");
        assert(logEntry.attr.command !== undefined, "Log entry should have 'attr.command' field");
        assert(logEntry.attr.command.pipeline !== undefined, "Log entry should have 'attr.command.pipeline' field");

        const modifyStage = logEntry.attr.command.pipeline[0].$modifyForLog;
        assert(modifyStage !== undefined, "$modifyForLog stage should be in logged pipeline");

        assert(modifyStage.logNote !== undefined, "Should have a logNote indicating modification");
        assert.eq(
            modifyStage.logNote,
            "Some fields were modified for logging",
            "logNote should indicate modifications were made",
        );

        assert(modifyStage.spec.largeArray !== undefined, "largeArray should exist in logged spec");
        assert.lte(modifyStage.spec.largeArray.length, 5, "Large array should be truncated to <= 5 elements");

        assert.eq(modifyStage.spec.smallArray.length, 3, "Small array should not be truncated");

        assert.eq(modifyStage.spec.normalField, "unchanged", "Normal field should not be modified");
    });

    it("should summarize large nested objects in slow query logs", function () {
        const largeObject = {};
        for (let i = 0; i < 20; i++) {
            largeObject[`field${i}`] = `value${i}`;
        }

        const comment = "summarize_large_objects_test";
        const pipeline = [
            {
                $modifyForLog: {
                    largeNestedObject: largeObject,
                    smallNestedObject: {a: 1, b: 2},
                },
            },
            {$limit: 1},
        ];

        const results = coll.aggregate(pipeline, {comment: comment}).toArray();
        assert.eq(results.length, 1, "Should return 1 document");

        const logs = getSlowQueryLogsByComment(db, comment);
        assert.gte(logs.length, 1, "Should find at least one slow query log entry");

        const logEntry = logs[0];
        assert(logEntry.attr !== undefined, "Log entry should have 'attr' field");
        assert(logEntry.attr.command !== undefined, "Log entry should have 'attr.command' field");
        assert(logEntry.attr.command.pipeline !== undefined, "Log entry should have 'attr.command.pipeline' field");

        const modifyStage = logEntry.attr.command.pipeline[0].$modifyForLog;
        assert(modifyStage !== undefined, "$modifyForLog stage should be in logged pipeline");

        assert(modifyStage.spec.largeNestedObject.summary !== undefined, "Large nested object should be summarized");
        assert(
            modifyStage.spec.largeNestedObject.summary.includes("20 fields"),
            "Summary should mention the number of fields",
        );

        assert.eq(modifyStage.spec.smallNestedObject.a, 1, "Small nested object should not be summarized");
    });

    it("should log explain with wrapper and modified pipeline in slow query logs", function () {
        // Verify that when running explain, the slow query log shows the full command.explain with
        // the inner aggregate's toBsonForLog-modified pipeline, not just the raw inner aggregate.
        const comment = "explain_modify_for_log_test";
        const pipeline = [
            {
                $modifyForLog: {
                    largeArray: Array.from({length: 20}, (_, i) => i),
                    normalField: "unchanged",
                },
            },
            {$limit: 1},
        ];

        // Use runCommand; explain().aggregate() in the shell returns the explain doc, not a cursor with .toArray().
        const explainResult = db.runCommand({
            explain: {
                aggregate: coll.getName(),
                pipeline: pipeline,
                cursor: {},
                comment: comment,
            },
            verbosity: "executionStats",
        });
        assert.commandWorked(explainResult, "Explain aggregate should succeed");

        const logs = getSlowQueryLogsByComment(db, comment);
        assert.gte(logs.length, 1, "Should find at least one slow query log entry");

        const logEntry = logs[0];
        assert(logEntry.attr !== undefined, "Log entry should have 'attr' field");
        assert(logEntry.attr.command !== undefined, "Log entry should have 'attr.command' field");

        // The op description must show the explain wrapper, not just the inner aggregate.
        assert(
            logEntry.attr.command.explain !== undefined,
            "Slow query log should show explain wrapper (command.explain), not just inner aggregate",
        );
        const explainedCmd = logEntry.attr.command.explain;
        assert(explainedCmd.aggregate !== undefined, "Explained command should be aggregate");
        assert(explainedCmd.pipeline !== undefined, "Explained command should have pipeline");

        // The pipeline in the log should show toBsonForLog modifications (truncated array, logNote).
        const modifyStage = explainedCmd.pipeline[0].$modifyForLog;
        assert(modifyStage !== undefined, "$modifyForLog stage should be in logged pipeline");
        assert(modifyStage.logNote !== undefined, "Should have logNote from toBsonForLog");
        assert.lte(
            modifyStage.spec.largeArray.length,
            5,
            "Large array in explain log should be truncated by toBsonForLog",
        );
        assert.eq(modifyStage.spec.normalField, "unchanged", "Normal field should not be modified");
    });
});
