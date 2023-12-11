/*
 * Test a nested $or query which reproduces SERVER-84013, a bug in the subplanner. This bug had to
 * do with the subplanner assuming that multiple invocations of MatchExpression::optimize() yielded
 * the same expressions, which turns out not to be the case. The queries in this regression test
 * excerise the $or -> $in rewrite which produce new $in expressions which themselves could be
 * further optimized.
 */

import {show} from "jstests/libs/golden_test.js";
import {resetCollection} from "jstests/query_golden/libs/utils.js";

const coll = db.server84013;

const docs = [
    {Country: {_id: "US"}, State: "California", City: "SanFrancisco"},
    {Country: {_id: "US"}, State: "NewYork", City: "Buffalo"},
];

const indexes = [{"Country._id": 1, "State": 1}];

resetCollection(coll, docs, indexes);

function runTest(pred) {
    jsTestLog(`find(${tojson(pred)})`);
    show(coll.find(pred));
}

runTest({
    "$or": [
        {"Country._id": "DNE"},
        {
            "Country._id": "US",
            "State": "California",
            "$or": [{"City": "SanFrancisco"}, {"City": {"$in": ["SanFrancisco"]}}]
        }
    ]
});

runTest({
    "$or": [
        {"Country._id": "DNE"},
        {
            "Country._id": "US",
            "State": "California",
            "$or": [
                {"City": "SanFrancisco"},
                {"City": {$in: ["SanFrancisco"]}},
                {"Country._id": "DNE"},
            ]
        },
    ]
});
