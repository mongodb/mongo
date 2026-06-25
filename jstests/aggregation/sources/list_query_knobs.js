/**
 * Tests for the $listQueryKnobs aggregation stage.
 *
 * @tags: [
 *      requires_fcv_90,
 *      do_not_wrap_aggregations_in_facets,
 * ]
 */
import {describe, it} from "jstests/libs/mochalite.js";

describe("$listQueryKnobs", function () {
    const adminDB = db.getSiblingDB("admin");

    it("returns non-empty results", function () {
        const results = adminDB.aggregate([{$listQueryKnobs: {}}]).toArray();
        assert.gt(results.length, 0, "expected at least one registered knob");
    });

    it("each document has name (string), pqsSettable (bool), and type (string)", function () {
        const results = adminDB.aggregate([{$listQueryKnobs: {}}]).toArray();
        const knobValueTypes = new Set(["int", "long long", "double", "bool", "enum"]);
        for (const doc of results) {
            assert.eq(typeof doc.name, "string", "expected name to be a string", {doc});
            assert.eq(typeof doc.pqsSettable, "boolean", "expected pqsSettable to be a bool", {
                doc,
            });
            assert(knobValueTypes.has(doc.type), "expected type to be a known knob value type", {
                doc,
            });
            if (doc.type === "enum") {
                assert(
                    Array.isArray(doc.allowedValues),
                    "expected allowedValues array on enum knob",
                    {doc},
                );
                assert.gt(doc.allowedValues.length, 0, "expected non-empty allowedValues", {doc});
                for (const v of doc.allowedValues) {
                    assert.eq(typeof v, "string", "expected each allowedValue to be a string", {
                        doc,
                    });
                }
            } else {
                assert.eq(
                    doc.allowedValues,
                    undefined,
                    "expected no allowedValues on non-enum knob",
                    {doc},
                );
            }
        }
    });

    it("rejects a non-empty spec", function () {
        assert.commandFailedWithCode(
            adminDB.runCommand({
                aggregate: 1,
                pipeline: [{$listQueryKnobs: {unexpected: 1}}],
                cursor: {},
            }),
            10491902,
        );
    });
});
