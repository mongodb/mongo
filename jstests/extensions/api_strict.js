/**
 * Tests that extension stages are not part of the Stable API: they are rejected under
 * 'apiStrict: true' in API Version 1, matching the built-in search stages they may replace.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {before, describe, it} from "jstests/libs/mochalite.js";

function runAgg(coll, pipeline, apiParams) {
    return coll.getDB().runCommand({aggregate: coll.getName(), pipeline, cursor: {}, ...apiParams});
}

describe("extension stages with apiStrict", function () {
    before(function () {
        this.coll = db.extension_api_strict;
        this.coll.drop();
        assert.commandWorked(this.coll.insert({_id: 1}));
    });

    it("is accepted without apiStrict", function () {
        assert.commandWorked(runAgg(this.coll, [{$testBar: {number: 42}}], {apiVersion: "1"}));
    });

    it("is accepted with apiStrict: false", function () {
        assert.commandWorked(
            runAgg(this.coll, [{$testBar: {number: 42}}], {apiVersion: "1", apiStrict: false}),
        );
    });

    it("is rejected with apiStrict: true", function () {
        assert.commandFailedWithCode(
            runAgg(this.coll, [{$testBar: {number: 42}}], {apiVersion: "1", apiStrict: true}),
            ErrorCodes.APIStrictError,
        );
    });

    it("is rejected with apiStrict: true in a subpipeline", function () {
        assert.commandFailedWithCode(
            runAgg(
                this.coll,
                [{$unionWith: {coll: this.coll.getName(), pipeline: [{$testBar: {number: 42}}]}}],
                {
                    apiVersion: "1",
                    apiStrict: true,
                },
            ),
            ErrorCodes.APIStrictError,
        );
    });
});
