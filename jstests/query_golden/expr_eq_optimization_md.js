// Test optimization of expressions of the form {$expr: {$eq: ["$a", 5]}}. When an index is available on the path "$a" and
// the equality constant is not null, the original $expr is satisfied by the index scan and can be removed from the Fetch filter.

import {tojsonMultiLineSortKeys, tojsonOnelineSortKeys} from "jstests/libs/golden_test.js";
import {code, line, linebreak, section} from "jstests/libs/query/pretty_md.js";
import {formatExplainRoot} from "jstests/libs/query/analyze_plan.js";

const collName = jsTestName();
const coll = db[collName];
coll.drop();

const docs = [
    {a: 0},
    {a: 1, b: 1, multiKeyField: [1, 5, 10]},
    {a: 2, b: 10, multiKeyField: [3, 4, 5]},
    {a: 3, b: 5, multiKeyField: [1, 10]},
    {a: 4, multiKeyField: 10},
    {a: null, b: 5},
];

section(`Inserted documents`);
code(tojson(docs));
assert.commandWorked(coll.insertMany(docs));

coll.createIndex({a: 1});
coll.createIndex({multiKeyField: 1});

function printResults(results) {
    let str = "[" + results.map(tojsonOnelineSortKeys).join(",\n ") + "]";
    code(str);
}

function outputPlanAndResults(pipeline) {
    const c = coll.aggregate(pipeline);
    const results = c.toArray();
    const explain = coll.explain().aggregate(pipeline);
    const winningPlan = formatExplainRoot(
        explain,
        true /* shouldFlatten */,
        ["queryShapeHash", "rejectedPlans"] /* fieldsToExclude */,
    );

    line("Parsed query");
    code(tojsonOnelineSortKeys(explain.queryPlanner.parsedQuery));

    line("Results");
    printResults(results);

    line("Summarized explain");
    code(tojsonMultiLineSortKeys(winningPlan));

    linebreak();
}

section(`Covered projection without fetch stage`);
outputPlanAndResults([{$match: {$expr: {$eq: ["$a", 3]}}}, {$project: {_id: 0, a: 1}}]);
outputPlanAndResults([{$match: {$expr: {$eq: [3, "$a"]}}}, {$project: {_id: 0, a: 1}}]);

section(`Index scan and fetch without filter.`);
outputPlanAndResults([{$match: {$expr: {$eq: ["$a", 3]}}}, {$project: {_id: 0, a: 1, b: 1}}]);

section(`Index scan and fetch with filter. The original $expr is kept in the fetch filter.`);
outputPlanAndResults([{$match: {$expr: {$eq: ["$a", null]}}}, {$project: {_id: 0, a: 1}}]);
outputPlanAndResults([{$match: {$expr: {$gt: ["$a", 1]}}}, {$project: {_id: 0, a: 1}}]);

section(`Multikey index: the optimization of $expr is not eligible.`);
outputPlanAndResults([{$match: {$expr: {$eq: ["$multiKeyField", 10]}}}, {$project: {_id: 0, multiKeyField: 1}}]);
