/**
 * Test the syntax of $fill.
 * @tags: [
 *   requires_fcv_53,
 *   do_not_wrap_aggregations_in_facets,
 * ]
 */

(function() {

"use strict";

load("jstests/libs/fixture_helpers.js");
load("jstests/libs/feature_flag_util.js");    // For isEnabled.
load("jstests/aggregation/extras/utils.js");  // For arrayEq.

if (!FeatureFlagUtil.isEnabled(db, "Fill")) {
    jsTestLog("Skipping as featureFlagFill is not enabled");
    return;
}
const coll = db[jsTestName()];
coll.drop();
const documents = [
    {_id: 1, linear: 1, other: 1, part: 1},
    {_id: 2, linear: 1, other: 1, part: 2},
    {_id: 3, linear: null, other: null, part: 1},
    {_id: 4, linear: null, other: null, part: 2},
    {_id: 5, linear: 5, other: 10, part: 1},
    {_id: 6, linear: 6, other: 2, part: 2},
    {_id: 7, linear: null, other: null, part: 1},
    {_id: 8, linear: 3, other: 5, part: 2},
    {_id: 9, linear: 7, other: 15, part: 1},
    {_id: 10, linear: null, other: null, part: 2}
];

assert.commandWorked(coll.insert(documents));

const testCases = [
    [
        // Verify $fill partitionBy works with type string and object.
        [{$fill: {sortBy: {_id: 1}, partitionBy: "$part", output: {linear: {method: "linear"}}}}],

        [
            {_id: 1, linear: 1, other: 1, part: 1},
            {_id: 3, linear: 3, other: null, part: 1},
            {_id: 5, linear: 5, other: 10, part: 1},
            {_id: 7, linear: 6, other: null, part: 1},
            {_id: 9, linear: 7, other: 15, part: 1},
            {_id: 2, linear: 1, other: 1, part: 2},
            {_id: 4, linear: 3.5, other: null, part: 2},
            {_id: 6, linear: 6, other: 2, part: 2},
            {_id: 8, linear: 3, other: 5, part: 2},
            {_id: 10, linear: null, other: null, part: 2},
        ]
    ],  // 0
    [
        [
            {$match: {part: 1}},
            {$project: {other: 0, part: 0}},
            {$fill: {sortBy: {_id: 1}, output: {linear: {method: "linear"}}}}
        ],
        [
            {_id: 1, linear: 1},
            {_id: 3, linear: 3},
            {_id: 5, linear: 5},
            {_id: 7, linear: 6},
            {_id: 9, linear: 7}
        ]
    ],  // 1
    [
        [
            {$project: {linear: 0, part: 0}},
            {$fill: {sortBy: {_id: 1}, output: {other: {method: "locf"}}}}
        ],
        [
            {_id: 1, other: 1},
            {_id: 2, other: 1},
            {_id: 3, other: 1},
            {_id: 4, other: 1},
            {_id: 5, other: 10},
            {_id: 6, other: 2},
            {_id: 7, other: 2},
            {_id: 8, other: 5},
            {_id: 9, other: 15},
            {_id: 10, other: 15}
        ]
    ],  // 2
    [
        [
            {$match: {part: 2}},
            {
                $fill: {
                    sortBy: {_id: 1},
                    output: {other: {method: "locf"}, linear: {method: "linear"}}
                }
            }
        ],
        [
            {_id: 2, linear: 1, other: 1, part: 2},
            {_id: 4, linear: 3.5, other: 1, part: 2},
            {_id: 6, linear: 6, other: 2, part: 2},
            {_id: 8, linear: 3, other: 5, part: 2},
            {_id: 10, linear: null, other: 5, part: 2}
        ]

    ],  // 3
    [
        [{
            $fill:
                {sortBy: {_id: 1}, output: {other: {method: "locf"}}, partitionByFields: ["part"]}
        }],
        [
            {_id: 1, linear: 1, other: 1, part: 1},
            {_id: 2, linear: 1, other: 1, part: 2},
            {_id: 3, linear: null, other: 1, part: 1},
            {_id: 4, linear: null, other: 1, part: 2},
            {_id: 5, linear: 5, other: 10, part: 1},
            {_id: 6, linear: 6, other: 2, part: 2},
            {_id: 7, linear: null, other: 10, part: 1},
            {_id: 8, linear: 3, other: 5, part: 2},
            {_id: 9, linear: 7, other: 15, part: 1},
            {_id: 10, linear: null, other: 5, part: 2}
        ]

    ],  // 4
    [
        [{
            $fill:
                {sortBy: {_id: 1}, output: {other: {method: "locf"}}, partitionBy: {part: "$part"}}
        }],
        [
            {_id: 1, linear: 1, other: 1, part: 1},
            {_id: 2, linear: 1, other: 1, part: 2},
            {_id: 3, linear: null, other: 1, part: 1},
            {_id: 4, linear: null, other: 1, part: 2},
            {_id: 5, linear: 5, other: 10, part: 1},
            {_id: 6, linear: 6, other: 2, part: 2},
            {_id: 7, linear: null, other: 10, part: 1},
            {_id: 8, linear: 3, other: 5, part: 2},
            {_id: 9, linear: 7, other: 15, part: 1},
            {_id: 10, linear: null, other: 5, part: 2}
        ]

    ],  // 5
    [
        [{
            $fill: {
                sortBy: {_id: 1},
                output: {linear: {method: "linear"}},
                partitionByFields: ["part"]
            }
        }],
        [
            {_id: 1, linear: 1, other: 1, part: 1},
            {_id: 3, linear: 3, other: null, part: 1},
            {_id: 5, linear: 5, other: 10, part: 1},
            {_id: 7, linear: 6, other: null, part: 1},
            {_id: 9, linear: 7, other: 15, part: 1},
            {_id: 2, linear: 1, other: 1, part: 2},
            {_id: 4, linear: 3.5, other: null, part: 2},
            {_id: 6, linear: 6, other: 2, part: 2},
            {_id: 8, linear: 3, other: 5, part: 2},
            {_id: 10, linear: null, other: null, part: 2}
        ]

    ],  // 6
    [
        [{
            $fill: {
                sortBy: {_id: 1},
                output: {linear: {method: "linear"}},
                partitionBy: {part: "$part"}
            }
        }],
        [
            {_id: 1, linear: 1, other: 1, part: 1},
            {_id: 3, linear: 3, other: null, part: 1},
            {_id: 5, linear: 5, other: 10, part: 1},
            {_id: 7, linear: 6, other: null, part: 1},
            {_id: 9, linear: 7, other: 15, part: 1},
            {_id: 2, linear: 1, other: 1, part: 2},
            {_id: 4, linear: 3.5, other: null, part: 2},
            {_id: 6, linear: 6, other: 2, part: 2},
            {_id: 8, linear: 3, other: 5, part: 2},
            {_id: 10, linear: null, other: null, part: 2}
        ]

    ],  // 7
    [
        [{
            $fill: {
                sortBy: {_id: 1},
                output: {other: {method: "locf"}, linear: {method: "linear"}},
                partitionByFields: ["part"]
            }
        }],
        [
            {_id: 1, linear: 1, other: 1, part: 1},
            {_id: 3, linear: 3, other: 1, part: 1},
            {_id: 5, linear: 5, other: 10, part: 1},
            {_id: 7, linear: 6, other: 10, part: 1},
            {_id: 9, linear: 7, other: 15, part: 1},
            {_id: 2, linear: 1, other: 1, part: 2},
            {_id: 4, linear: 3.5, other: 1, part: 2},
            {_id: 6, linear: 6, other: 2, part: 2},
            {_id: 8, linear: 3, other: 5, part: 2},
            {_id: 10, linear: null, other: 5, part: 2}
        ]
    ],  // 8
    // Test with first element in partition having a null fill field.
    [
        [
            {$set: {linear: {$cond: [{$eq: ["$linear", 1]}, null, "$linear"]}}},
            {
                $fill: {
                    sortBy: {_id: 1},
                    output: {other: {method: "locf"}, linear: {method: "linear"}},
                    partitionByFields: ["part"]
                }
            }
        ],
        [
            {_id: 1, linear: null, other: 1, part: 1},
            {_id: 3, linear: null, other: 1, part: 1},
            {_id: 5, linear: 5, other: 10, part: 1},
            {_id: 7, linear: 6, other: 10, part: 1},
            {_id: 9, linear: 7, other: 15, part: 1},
            {_id: 2, linear: null, other: 1, part: 2},
            {_id: 4, linear: null, other: 1, part: 2},
            {_id: 6, linear: 6, other: 2, part: 2},
            {_id: 8, linear: 3, other: 5, part: 2},
            {_id: 10, linear: null, other: 5, part: 2}
        ]
    ],  // 9
    // Test $fill with arbitrary values.
    [
        [
            {$match: {part: 1}},
            {$project: {linear: 0, part: 0}},
            {$fill: {sortBy: {_id: 1}, output: {other: {value: "$_id"}}}}
        ],
        [
            {_id: 1, other: 1},
            {_id: 3, other: 3},
            {_id: 5, other: 10},
            {_id: 7, other: 7},
            {_id: 9, other: 15},
        ]
    ],  // 10
    [
        [
            {$match: {part: 1}},
            {$project: {linear: 0, part: 0}},
            {$fill: {sortBy: {_id: 1}, output: {other: {value: -1}}}}
        ],
        [
            {_id: 1, other: 1},
            {_id: 3, other: -1},
            {_id: 5, other: 10},
            {_id: 7, other: -1},
            {_id: 9, other: 15},
        ]
    ],  // 11
    [
        [
            {$match: {part: 1}},
            {
                $fill: {
                    sortBy: {_id: 1},
                    output: {other: {value: -1}, linear: {value: {$add: ["$part", 1]}}}
                }
            },
            {$project: {part: 0}},
        ],
        [
            {_id: 1, other: 1, linear: 1},
            {_id: 3, other: -1, linear: 2},
            {_id: 5, other: 10, linear: 5},
            {_id: 7, other: -1, linear: 2},
            {_id: 9, other: 15, linear: 7},
        ]

    ],  // 12
    // Verify behavior if the filling expression can evaluate to missing or null.
    [
        [
            {$match: {part: 1}},
            {$project: {part: 0}},
            {$unionWith: {pipeline: [{$documents: [{_id: 2}, {_id: 4}]}]}},
            {$fill: {sortBy: {_id: 1}, output: {other: {value: "$linear"}}}},
            {$project: {linear: 0}},
        ],
        [
            {_id: 1, other: 1},
            {_id: 2},
            {_id: 3, other: null},
            {_id: 4},
            {_id: 5, other: 10},
            {_id: 7, other: null},
            {_id: 9, other: 15},
        ]

    ],  // 13

];

for (let i = 0; i < testCases.length; i++) {
    const result = coll.aggregate(testCases[i][0]).toArray();
    assertArrayEq(
        {actual: result, expected: testCases[i][1], extraErrorMsg: " during testCase " + i});
}
})();
