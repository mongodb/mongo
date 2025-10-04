/*
 * Test that a query eligible for partial index and gets cached does not affect the results of a
 * query with the same shape but ineligible for the partial index. This file is a regression
 * testcase for SERVER-102825 and SERVER-106023.
 */

import {code, line, section, subSection} from "jstests/libs/pretty_md.js";
import {getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {outputPlanCacheStats} from "jstests/libs/query/golden_test_utils.js";

const coll = db[jsTestName()];

function runFindTest(pred) {
    subSection("Query");
    code(`${tojson(pred)}`);
    subSection("Results");
    const results = coll.find(pred).toArray();
    code(tojson(results));
}

function runAggTest(pipeline) {
    subSection("Aggregation");
    code(`${tojson(pipeline)}`);
    subSection("Results");
    const results = coll.aggregate(pipeline).toArray();
    code(tojson(results));
}

const docs = [{_id: 1, a: 0}];

{
    // Reproducing SERVER-102825
    coll.drop();
    assert.commandWorked(coll.insert(docs));
    section("Collection setup");
    line("Inserting documents");
    code(tojson(docs));

    section("Queries with no indexes");
    const q1 = {$or: [{a: 1}, {a: {$lte: "a string"}}], _id: {$lte: 5}};
    const q2 = {$or: [{a: 1}, {a: {$lte: 10}}], _id: {$lte: 5}};
    runFindTest(q1);
    runFindTest(q2);

    const partialFilter = {$or: [{a: 1}, {a: {$lte: "a string"}}]};
    assert.commandWorked(coll.createIndex({a: 1}, {partialFilterExpression: partialFilter}));
    section("Index setup");
    line("Creating Index");
    code(
        tojson({
            a: 1,
            partialFilterExpression: partialFilter,
        }),
    );

    section("Queries with partial index");
    // Ensure q1 is cached which uses the partial filter.
    for (let i = 0; i < 3; i++) {
        runFindTest(q1);
    }

    subSection("Explain");
    // Assert that q1 uses the partial index
    const explain = coll.find(q1).explain();
    code(tojson(getWinningPlanFromExplain(explain)));
    subSection("Plan cache");
    line("Verifying that the plan cache contains an entry with the partial index");
    outputPlanCacheStats(coll);

    // q2 should not used the partial index and return the document.
    line("Verify that 2nd query does not use cached partial index plan and returns the correct document");
    runFindTest(q2);
}

{
    // Reproducing SERVER-106023
    coll.drop();

    assert.commandWorked(coll.insert(docs));
    section("Collection setup");
    line("Inserting documents");
    code(tojson(docs));

    const partialFilter = {
        $or: [{a: {$lt: "a"}}, {_id: {$eq: 0}, a: {$eq: 0}}],
    };

    const indexSpecs = [{a: 1}, {a: 1, m: 1}, {b: 1, a: 1}];

    indexSpecs.forEach((spec) => {
        line("Creating Index");
        code(tojson({"spec": spec, "partialFilterExpression": partialFilter}));
        assert.commandWorked(coll.createIndex(spec, {partialFilterExpression: partialFilter}));
    });

    const p1 = [
        {
            $match: {
                $or: [
                    {a: {$lt: "a"}}, // This branch matches the partial filter
                    {_id: {$eq: 0}, a: {$eq: -1}},
                ],
            },
        },
        {$sort: {b: 1}},
    ];
    const p2 = [
        {
            $match: {
                $or: [
                    {a: {$lt: 1}},
                    {_id: {$eq: 0}, a: {$eq: 0}}, // This branch matches the partial filter
                ],
            },
        },
        {$sort: {b: 1}},
    ];

    runAggTest(p1);
    runAggTest(p1);
    runAggTest(p1);

    subSection("Plan cache");
    line("Verifying that the plan cache contains an entry with the partial index");
    outputPlanCacheStats(coll);

    line("Verify that 2nd pipeline does not use cached partial index plan and returns the correct document");
    runAggTest(p2);
}
