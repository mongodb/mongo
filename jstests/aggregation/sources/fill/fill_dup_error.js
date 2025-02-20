// Ensure that if sort values are duplicated in different partition $fill succeeds.

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];
coll.drop();
const documents = [
    {_id: 1, date: ISODate("2021-03-08"), restaurant: "Joe's Pizza", score: 90},
    {_id: 2, date: ISODate("2021-04-10"), restaurant: "Ted's Bistro", score: 80},
    {_id: 3, date: ISODate("2021-03-08"), restaurant: "Sally's Deli", score: 75}
];

assert.commandWorked(coll.insert(documents));

const testCases = [
    [
        // Verify $fill partitionBy works with type string and object.
        [
            {
                $fill: {
                    sortBy: {date: 1},
                    partitionBy: {"restaurant": "$restaurant"},
                    output: {"score": {method: "linear"}}
                }
            },
            {$project: {_id: 1}},
        ],
        [
            {_id: 1},
            {_id: 3},
            {_id: 2},
        ]
    ],  // 0
];

for (let i = 0; i < testCases.length; i++) {
    const result = coll.aggregate(testCases[i][0]).toArray();
    assertArrayEq(
        {actual: result, expected: testCases[i][1], extraErrorMsg: " during testCase " + i});
}
