/**
 * Test that $densify can combine sort stages.
 * @tags: [
 *   requires_fcv_53,
 *   requires_pipeline_optimization,
 *   do_not_wrap_aggregations_in_facets,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");
load("jstests/libs/feature_flag_util.js");    // For isEnabled.
load("jstests/aggregation/extras/utils.js");  // For getExplainedPipelineFromAggregation.

const coll = db[jsTestName()];
coll.drop();

const documents = [
    {_id: 0, val: 0},
    {_id: 1},
];

assert.commandWorked(coll.insert(documents));
const testCases = [
    // If there are no partitions densify can combine a smaller or equal sort.
    [
        [{$densify: {field: "val", range: {step: 1, bounds: "full"}}}, {$sort: {val: 1}}],
        [
            {"$sort": {"sortKey": {"val": 1}}},
            {
                "$_internalDensify": {
                    "field": "val",
                    "partitionByFields": [],
                    "range": {"step": 1, "bounds": "full"}
                }
            },
        ]
    ],  // 0
    [
        [{$densify: {field: "val", range: {step: 1, bounds: "full"}}}, {$sort: {other: 1, val: 1}}],
        [
            {
                "$sort": {
                    "sortKey": {
                        "val": 1,
                    }
                }
            },
            {
                "$_internalDensify": {
                    "field": "val",
                    "partitionByFields": [],
                    "range": {"step": 1, "bounds": "full"}
                }
            },
            {
                "$sort": {
                    "sortKey": {
                        "other": 1,
                        "val": 1,
                    }
                }
            },
        ]
    ],  // 1
    [
        [{$densify: {field: "val", range: {step: 1, bounds: [-10, 10]}}}, {$sort: {val: 1}}],
        [
            {
                "$sort": {
                    "sortKey": {
                        "val": 1,
                    }
                }
            },
            {
                "$_internalDensify": {
                    "field": "val",
                    "partitionByFields": [],
                    "range": {"step": 1, "bounds": [-10, 10]}
                }
            },
        ]
    ],  // 2
    // With partitions and range: "full" sorts cannot be combined.
    [
        [
            {
                $densify:
                    {field: "val", range: {step: 1, bounds: "full"}, partitionByFields: ["random"]}
            },
            {$sort: {val: 1}}
        ],
        [
            {"$sort": {"sortKey": {"val": 1}}},
            {
                "$_internalDensify": {
                    "field": "val",
                    "partitionByFields": ["random"],
                    "range": {"step": 1, "bounds": "full"}
                }
            },
            {"$sort": {"sortKey": {"val": 1}}}
        ]
    ],  // 3

    // Partitions with non-full bounds means the generated sort order is preserved. Combine if the
    // sorts match.
    [
        [
            {
                $densify: {
                    field: "val",
                    range: {step: 1, bounds: "partition"},
                    partitionByFields: ["random"]
                }
            },
            {$sort: {random: 1, val: 1}}
        ],
        [
            {"$sort": {"sortKey": {"random": 1, "val": 1}}},
            {
                "$_internalDensify": {
                    "field": "val",
                    "partitionByFields": ["random"],
                    "range": {"step": 1, "bounds": "partition"}
                }
            },
        ]
    ],  // 4
    [
        [
            {
                $densify: {
                    field: "val",
                    range: {step: 1, bounds: "partition"},
                    partitionByFields: ["random"]
                }
            },
            {$sort: {other: 1, val: 1}}
        ],
        [
            {"$sort": {"sortKey": {"random": 1, "val": 1}}},
            {
                "$_internalDensify": {
                    "field": "val",
                    "partitionByFields": ["random"],
                    "range": {"step": 1, "bounds": "partition"}
                }
            },
            {"$sort": {"sortKey": {"other": 1, "val": 1}}}
        ]
    ],  // 5
    [
        [
            {
                $densify: {
                    field: "val",
                    range: {step: 1, bounds: [-10, 10]},
                    partitionByFields: ["random"]
                }
            },
            {$sort: {random: 1, val: 1}},
        ],
        [
            {"$sort": {"sortKey": {"random": 1, "val": 1}}},
            {
                "$_internalDensify": {
                    "field": "val",
                    "partitionByFields": ["random"],
                    "range": {"step": 1, "bounds": [-10, 10]}
                }
            },
        ]
    ],  // 6
    [
        [
            {
                $densify: {
                    field: "val",
                    range: {step: 1, bounds: [-10, 10]},
                    partitionByFields: ["random"]
                }
            },
            {$sort: {other: 1, val: 1}}
        ],
        [
            {"$sort": {"sortKey": {"random": 1, "val": 1}}},
            {
                "$_internalDensify": {
                    "field": "val",
                    "partitionByFields": ["random"],
                    "range": {"step": 1, "bounds": [-10, 10]}
                }
            },
            {"$sort": {"sortKey": {"other": 1, "val": 1}}}
        ]
    ],  // 7
    // Test that a following, stricter, sort is preserved and not combined.
    [
        [
            {
                $densify: {
                    field: "val",
                    range: {step: 1, bounds: "partition"},
                    partitionByFields: ["random"]
                }
            },
            {$sort: {random: 1, val: 1, _id: 1}}
        ],
        [
            {"$sort": {"sortKey": {"random": 1, "val": 1}}},
            {
                "$_internalDensify": {
                    "field": "val",
                    "partitionByFields": ["random"],
                    "range": {"step": 1, "bounds": "partition"}
                }
            },
            {"$sort": {"sortKey": {"random": 1, "val": 1, "_id": 1}}},
        ]
    ],  // 8
    // Demonstrate that multiple stages that combine sorts may still wind up with an extra sort at
    // the end.
    [
        [
            {
                $densify: {
                    field: "val",
                    range: {step: 1, bounds: "partition"},
                    partitionByFields: ["random"]
                }
            },
            {
                $setWindowFields:
                    {partitionBy: "$random", sortBy: {"val": 1}, output: {val: {$sum: "$val"}}}
            },
            {$sort: {random: 1, val: 1}}
        ],
        [
            {"$sort": {"sortKey": {"random": 1, "val": 1}}},
            {
                "$_internalDensify": {
                    "field": "val",
                    "partitionByFields": ["random"],
                    "range": {"step": 1, "bounds": "partition"}
                }
            },
            {
                "$_internalSetWindowFields": {
                    "partitionBy": "$random",
                    "sortBy": {"val": 1},
                    "output": {
                        "val":
                            {"$sum": "$val", "window": {"documents": ["unbounded", "unbounded"]}}
                    }
                }
            },
            {$sort: {sortKey: {random: 1, val: 1}}}
        ]
    ],  // 9
    // Test that if the densify generated sort is preceded by an additional sort, we optimize based
    // on the densify sort not the preceding one.
    [
        [
            {$sort: {val: 1, other: 1}},
            {$densify: {field: "val", range: {step: 1, bounds: "full"}}},
            {$sort: {val: 1, other: 1}}
        ],
        [
            {"$sort": {"sortKey": {"val": 1}}},
            {
                "$_internalDensify": {
                    "field": "val",
                    "partitionByFields": [],
                    "range": {"step": 1, "bounds": "full"}
                }
            },
            {"$sort": {"sortKey": {"val": 1, "other": 1}}},
        ]

    ],  // 10
];
for (let i = 0; i < testCases.length; i++) {
    let result = getExplainedPipelineFromAggregation(db, coll, testCases[i][0]);

    assert(anyEq(result, testCases[i][1]),
           "Test case " + i + " failed.\n" +
               "Expected:\n" + tojson(testCases[i][1]) + "\nGot:\n" + tojson(result));
}
})();
