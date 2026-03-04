/**
 * Tests that extension stages can modify their BSON representation in toBsonForLog() and that
 * these modifications appear correctly in system.profile.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   assumes_against_mongod_not_mongos,
 *   does_not_support_stepdowns,
 *   requires_profiling,
 * ]
 */

import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();
const coll = db.getCollection(collName);

describe("$modifyForLog in system.profile", function () {
    before(function () {
        db.dropDatabase();
        assert.commandWorked(
            coll.insertMany([
                {_id: 0, value: "a"},
                {_id: 1, value: "b"},
                {_id: 2, value: "c"},
            ]),
        );

        assert.commandWorked(db.setProfilingLevel(2));
    });

    after(function () {
        assert.commandWorked(db.setProfilingLevel(0));
        db.dropDatabase();
    });

    it("should show truncated pipeline in system.profile", function () {
        const largeArray = Array.from({length: 100}, (_, i) => i);
        const comment = "profile_truncate_test";
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

        coll.aggregate(pipeline, {comment: comment}).toArray();

        const profileEntry = getLatestProfilerEntry(db, {"command.comment": comment});

        assert(
            profileEntry.command.pipeline !== undefined,
            "Profile entry should have pipeline: " + tojson(profileEntry),
        );

        const modifyStage = profileEntry.command.pipeline[0].$modifyForLog;
        assert(modifyStage !== undefined, "$modifyForLog should appear in profiled pipeline: " + tojson(profileEntry));

        assert.eq(
            modifyStage.logNote,
            "Some fields were modified for logging",
            "logNote should indicate modifications: " + tojson(modifyStage),
        );
        assert.lte(modifyStage.spec.largeArray.length, 5, "Large array should be truncated: " + tojson(modifyStage));
        assert.eq(modifyStage.spec.smallArray.length, 3, "Small array should not be truncated: " + tojson(modifyStage));
        assert.eq(
            modifyStage.spec.normalField,
            "unchanged",
            "Normal field should not be modified: " + tojson(modifyStage),
        );
    });

    it("should show summarized objects in system.profile", function () {
        const largeObject = {};
        for (let i = 0; i < 20; i++) {
            largeObject[`field${i}`] = `value${i}`;
        }

        const comment = "profile_summarize_test";
        const pipeline = [
            {
                $modifyForLog: {
                    largeNestedObject: largeObject,
                    smallNestedObject: {a: 1, b: 2},
                },
            },
            {$limit: 1},
        ];

        coll.aggregate(pipeline, {comment: comment}).toArray();

        const profileEntry = getLatestProfilerEntry(db, {"command.comment": comment});

        const modifyStage = profileEntry.command.pipeline[0].$modifyForLog;
        assert(modifyStage !== undefined, "$modifyForLog should appear in profiled pipeline: " + tojson(profileEntry));

        assert(
            modifyStage.spec.largeNestedObject.summary !== undefined,
            "Large nested object should be summarized: " + tojson(modifyStage),
        );
        assert(
            modifyStage.spec.largeNestedObject.summary.includes("20 fields"),
            "Summary should mention the number of fields: " + tojson(modifyStage),
        );
        assert.eq(
            modifyStage.spec.smallNestedObject.a,
            1,
            "Small nested object should not be summarized: " + tojson(modifyStage),
        );
    });
});
