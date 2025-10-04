// Test rewrite to have a $in of the form {$expr: {$in: [<const>, <$field-path>]}} use an index.

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {tojsonMultiLineSortKeys, tojsonOnelineSortKeys} from "jstests/libs/golden_test.js";
import {code, line, linebreak, section, subSection} from "jstests/libs/pretty_md.js";
import {formatExplainRoot} from "jstests/libs/query/analyze_plan.js";

const paramName = "internalQueryExtraPredicateForReversedIn";
const cachedParamValue = assert.commandWorked(db.adminCommand({getParameter: 1, [paramName]: 1}));

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(
    coll.insertMany([
        {m: []},
        {m: [[]]},
        {m: [[[]]]},
        {a: 1, m: [1]},
        {a: 2, m: [1, 2, 3]},
        {m: [1, 2]},
        {m: [2]},
        {m: [5, 2, 3, 6]},
        {m: [5, 2, 1, 3, 6]},
        {m: [4, 5, 6, 10]},
        {m: [4, 5, 6, null, 10]},
        {m: [null, null, null]},
        // Nested array cases.
        {m: [[1]]},
        {m: [[[1]]]},
        {m: [[2]]},
        {m: [[1, 2]]},
        {m: [[[1, 2]]]},
        {m: [[1, 2, 3, 4]]},
        {
            m: [
                [1, 2, 3, 4],
                [1, 2],
            ],
        },
        {m: [[2, 1]]},
        {m: [[1], [2]]},
        {m: [[[1]], [[2]]]},
        {m: [[[1], [2]]]},
        {m: [[null], null]},
        {m: [[[null]]]},
        // Object cases
        {m: [{}]},
        {m: [[{}]]},
        {m: [{a: 1}]},
        {m: [[{a: 1}]]},
        {m: [{a: [1]}]},
        {m: [{a: []}]},
        {m: [{a: null}]},
        {m: [{a: {}}]},
        {m: [{a: 1, b: 1}]},
        // String & regex
        {m: ["a", "b", "c"]},
        {m: ["abc"]},
        {m: ["ghi", "abc", "def"]},
        {m: ["aBC"]},
        {m: ["ABC"]},
        {m: [/abc/]},
        {m: [/a/]},
    ]),
);

assert.commandWorked(coll.createIndex({m: 1}));
assert.commandWorked(coll.createIndex({"m.a": 1}));

function printResults(results) {
    let str = "[" + results.map(tojsonOnelineSortKeys).join(",\n ") + "]";
    code(str);
}

function outputPlanAndResults(filter) {
    // Note: we can't get a covered plan here, because our index is/must be multikey.
    // If the index is not multikey, the query will error out in any case.
    const c = coll.find(filter, {_id: 0});
    const results = c.toArray();
    const explain = c.explain();
    const flatPlan = formatExplainRoot(explain);

    line("Find results");
    printResults(results);

    line("Parsed find query");
    code(tojsonOnelineSortKeys(explain.queryPlanner.parsedQuery));

    line("Summarized explain");
    code(tojsonMultiLineSortKeys(flatPlan));

    linebreak();
    return results;
}

function validateResultsSame(filter) {
    section(`Find filter`);
    code(tojsonOnelineSortKeys(filter));

    subSection("Query knob off");
    assert.commandWorked(db.adminCommand({setParameter: 1, [paramName]: false}));
    const resOff = outputPlanAndResults(filter);

    subSection("Query knob on");
    assert.commandWorked(db.adminCommand({setParameter: 1, [paramName]: true}));
    const resOn = outputPlanAndResults(filter);
    assertArrayEq({expected: resOff, actual: resOn});
}

function validateError(filter) {
    assert.commandWorked(db.adminCommand({setParameter: 1, [paramName]: false}));
    assert.commandFailedWithCode(db.runCommand({find: coll.getName(), filter}), [40081, 5153700]);

    assert.commandWorked(db.adminCommand({setParameter: 1, [paramName]: true}));
    assert.commandFailedWithCode(db.runCommand({find: coll.getName(), filter}), [40081, 5153700]);
}

try {
    // Tests for "$m".
    validateResultsSame({$expr: {$in: [1, "$m"]}});
    validateResultsSame({$expr: {$in: [null, "$m"]}});
    validateResultsSame({$expr: {$in: [[], "$m"]}});
    validateResultsSame({$expr: {$in: [[1], "$m"]}});
    validateResultsSame({$expr: {$in: [[2], "$m"]}});
    validateResultsSame({$expr: {$in: [[null], "$m"]}});
    validateResultsSame({$expr: {$in: [[1, 2], "$m"]}});
    validateResultsSame({$expr: {$in: [[[1]], "$m"]}});
    validateResultsSame({$expr: {$in: [[[1, 2]], "$m"]}});
    validateResultsSame({$expr: {$in: [[[[1]]], "$m"]}});
    validateResultsSame({$expr: {$in: [{}, "$m"]}});
    validateResultsSame({$expr: {$in: [[{}], "$m"]}});
    validateResultsSame({$expr: {$in: [{a: 1}, "$m"]}});
    validateResultsSame({$expr: {$in: [[{a: 1}], "$m"]}});
    validateResultsSame({$expr: {$in: [{a: 1, b: 1}, "$m"]}});
    validateResultsSame({$expr: {$in: [[{}], "$m"]}});
    validateResultsSame({$expr: {$in: ["a", "$m"]}});
    validateResultsSame({$expr: {$in: ["ab", "$m"]}});
    validateResultsSame({$expr: {$in: ["abc", "$m"]}});
    validateResultsSame({$expr: {$in: [["a"], "$m"]}});
    validateResultsSame({$expr: {$in: [["ab"], "$m"]}});
    validateResultsSame({$expr: {$in: [["abc"], "$m"]}});
    // Note: we don't test with regex outside an array because we don't do the rewrite in that case.
    validateResultsSame({$expr: {$in: [[/a/], "$m"]}});
    validateResultsSame({$expr: {$in: [[/b/], "$m"]}});
    validateResultsSame({$expr: {$in: [[/abc/], "$m"]}});
    validateResultsSame({$expr: {$or: [{$in: [1, "$m"]}]}});
    validateResultsSame({$expr: {$or: [{$in: [1, "$m"]}, {$in: [2, "$m"]}]}});
    validateResultsSame({$expr: {$or: [{$in: [1, "$m"]}, {$in: [2, "$m"]}, {$in: ["$a", [1, 2, 10]]}]}});
    validateResultsSame({$expr: {$and: [{$in: ["$a", [1, 2]]}, {$or: [{$in: [1, "$m"]}, {$in: [2, "$m"]}]}]}});
    validateResultsSame({$expr: {$and: [{$or: [{$in: [1, "$m"]}, {$in: [2, "$m"]}]}, {$in: ["$a", [null]]}]}});

    // Tests for "$m.a".
    validateResultsSame({$expr: {$in: [1, "$m.a"]}});
    validateResultsSame({$expr: {$in: [[1], "$m.a"]}});
    validateResultsSame({$expr: {$in: [[[1]], "$m.a"]}});
    validateResultsSame({$expr: {$in: [[], "$m.a"]}});
    validateResultsSame({$expr: {$in: [[[]], "$m.a"]}});
    validateResultsSame({$expr: {$in: [{}, "$m.a"]}});
    validateResultsSame({$expr: {$in: [[{}], "$m.a"]}});
    validateResultsSame({$expr: {$in: [null, "$m.a"]}});
    validateResultsSame({$expr: {$in: [[null], "$m.a"]}});

    // Validate error behaviour when the "bad" document is included in results.
    const badId = "force failure due to non-array $m";
    assert.commandWorked(coll.insertOne({_id: badId}));
    validateError({$expr: {$in: [null, "$m"]}});
    validateError({$expr: {$or: [{$in: [null, "$m"]}, {$in: [1, "$m"]}, {$in: [2, "$m"]}]}});
    // Note: this won't fail with an IXSCAN, but will with a COLLSCAN.
    // We accept this because we have precedent.
    // validateError({$expr: {$in: [1, "$m"]}});
} finally {
    assert.commandWorked(db.adminCommand({setParameter: 1, [paramName]: cachedParamValue[paramName]}));
}
