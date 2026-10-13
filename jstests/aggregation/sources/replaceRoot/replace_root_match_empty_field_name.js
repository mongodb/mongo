/**
 * Ensure that $replaceRoot followed by a $match on a path whose leading component is
 * an empty field name returns correct results.
 */

import {arrayEq} from "jstests/aggregation/extras/utils.js";
import {before, describe, it} from "jstests/libs/mochalite.js";

describe("$replaceWith followed by $match on an empty-named field", function () {
    const subHasEmptyNamedSubdoc = {_id: 0, sub: {"": {a: 2}}};
    const subHasEmptyNamedScalar = {_id: 1, sub: {"": 2}};
    const subHasOtherValue = {_id: 2, sub: {"": {a: 99}}};
    const emptyNamedFieldAtTopLevelOnly = {_id: 3, "": {a: 2}, sub: {x: 1}};
    const subHasNestedEmptyNames = {_id: 4, sub: {"": {"": {a: 7}}}};
    const subHasPlainField = {_id: 5, sub: {a: 2}};

    const coll = db[jsTestName()];

    before(function () {
        coll.drop();
        assert.commandWorked(
            coll.insert([
                subHasEmptyNamedSubdoc,
                subHasEmptyNamedScalar,
                subHasOtherValue,
                emptyNamedFieldAtTopLevelOnly,
                subHasNestedEmptyNames,
                subHasPlainField,
            ]),
        );
    });

    // Asserts that 'pipeline' returns exactly the 'sub' values of 'expected'.
    function assertResults(pipeline, expected) {
        const actual = coll.aggregate(pipeline).toArray();
        assert(
            arrayEq(
                expected.map((doc) => doc["sub"]),
                actual,
            ),
            "unexpected results",
            {pipeline, expected, actual},
        );
    }

    it("matches a subfield of an empty-named field", function () {
        // The new root of 'subHasEmptyNamedSubdoc' is {"": {a: 2}}, so '.a' is 2 and it matches.
        assertResults([{$replaceWith: "$sub"}, {$match: {".a": 2}}], [subHasEmptyNamedSubdoc]);
    });

    it("matches a negated predicate on a subfield of an empty-named field", function () {
        const expected = [
            subHasEmptyNamedScalar,
            subHasOtherValue,
            emptyNamedFieldAtTopLevelOnly,
            subHasNestedEmptyNames,
            subHasPlainField,
        ];
        assertResults([{$replaceWith: "$sub"}, {$match: {".a": {$ne: 2}}}], expected);
    });

    it("matches an $exists predicate on a subfield of an empty-named field", function () {
        assertResults(
            [{$replaceWith: "$sub"}, {$match: {".a": {$exists: true}}}],
            [subHasEmptyNamedSubdoc, subHasOtherValue],
        );
    });

    it("matches the empty-named field itself", function () {
        assertResults([{$replaceWith: "$sub"}, {$match: {"": {a: 2}}}], [subHasEmptyNamedSubdoc]);
    });

    it("matches a path with several empty-named components", function () {
        assertResults([{$replaceWith: "$sub"}, {$match: {"..a": 7}}], [subHasNestedEmptyNames]);
    });

    it("matches a range predicate on a subfield of an empty-named field", function () {
        assertResults([{$replaceWith: "$sub"}, {$match: {".a": {$gt: 50}}}], [subHasOtherValue]);
    });

    it("matches an empty-named predicate conjoined with a plain one", function () {
        assertResults(
            [{$replaceWith: "$sub"}, {$match: {$and: [{".a": 2}, {"": {$type: "object"}}]}}],
            [subHasEmptyNamedSubdoc],
        );
    });

    it("returns the same results with $replaceRoot as with $replaceWith", function () {
        assertResults(
            [{$replaceRoot: {newRoot: "$sub"}}, {$match: {".a": 2}}],
            [subHasEmptyNamedSubdoc],
        );
    });

    // TODO SERVER-55595 Update this test file if we resolve that $match should error or $project should support empty field names.
    // Demonstrate how $project differs from $replaceRoot.
    it("$project rejects empty field names", function () {
        assert.throwsWithCode(() => coll.aggregate([{$project: {"": 1}}]).toArray(), 40352);
        assert.throwsWithCode(() => coll.aggregate([{$project: {".a": 1}}]).toArray(), 15998);
    });
});
