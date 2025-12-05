/**
 * Validates that optimizations from $sort+$group to distinct scan and index scan plans are
 * performed when expected and return valid output.
 *
 * @tags: [
 *   requires_fcv_81,
 * ]
 */

import {
    assertPlanUsesDistinctScan,
    assertPlanUsesIndexScan
} from "jstests/libs/query/group_to_distinct_scan_utils.js";

const docs = [
    {
        kind: "ice hockey",
        season: "winter",
        year: 1919,
    },
    {
        kind: "olympic",
        season: "winter",
        year: 1920,
    },
    {
        kind: "olympic",
        season: "summer",
        year: 1922,
    },
    {
        kind: "olympic",
        season: "winter",
        year: 1924,
    },
    {
        kind: "olympic",
        season: "summer",
        year: 1926,
    },
    {
        kind: "olympic",
        season: "winter",
        year: 1928,
    },
    {
        kind: "olympic",
        season: "summer",
        year: 1930,
    },
    {
        kind: "ice hockey",
        season: "winter",
        year: 1920,
    },
    {
        kind: "ice hockey",
        season: "winter",
        year: 1921,
    },
    {
        kind: "ice hockey",
        season: "winter",
        year: 1922,
    },
    {
        kind: "ice hockey",
        season: "winter",
        year: 1923,
    },
];

const indexes = [
    {kind: 1, season: 1, year: 1},
    {season: 1, year: 1},
];

// TODO SERVER-113145: DISTINCT_SCAN optimization cases enabled when
// featureFlagShardFilteringDistinctScan is enabled.
const alwaysEnabledDistictScanTestCases = [
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {
                $group: {
                    _id: "$season",
                    year: {$bottom: {output: "$year", sortBy: {season: 1, year: 1}}}
                }
            },
        ],
        index: {season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {
                $group:
                    {_id: "$season", year: {$top: {output: "$year", sortBy: {season: 1, year: 1}}}}
            },
        ],
        index: {season: 1, year: 1},
    },
];

// TODO SERVER-113145: DISTINCT_SCAN optimization cases disabled when
// featureFlagShardFilteringDistinctScan is enabled.
const distictScanTestCases = [
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {$group: {_id: "$season", year: {$bottom: {output: "$year", sortBy: {year: 1}}}}},
        ],
        index: {season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {$group: {_id: "$season", year: {$bottom: {output: "$year", sortBy: {year: -1}}}}},
        ],
        index: {season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {$group: {_id: "$season", year: {$top: {output: "$year", sortBy: {year: 1}}}}},
        ],
        index: {season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {$group: {_id: "$season", year: {$top: {output: "$year", sortBy: {year: -1}}}}},
        ],
        index: {season: 1, year: 1},
    },
    // Multiple accumulators
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {
                $group: {
                    _id: "$season",
                    year: {$bottom: {output: "$year", sortBy: {year: 1}}},
                    kind: {$bottom: {output: "$kind", sortBy: {year: 1}}},
                },
            },
        ],
        index: {season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {
                $group: {
                    _id: "$season",
                    year: {$bottom: {output: "$year", sortBy: {year: -1}}},
                    kind: {$bottom: {output: "$kind", sortBy: {year: -1}}},
                },
            },
        ],
        index: {season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {
                $group: {
                    _id: "$season",
                    year: {$top: {output: "$year", sortBy: {year: 1}}},
                    kind: {$top: {output: "$kind", sortBy: {year: 1}}},
                },
            },
        ],
        index: {season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {
                $group: {
                    _id: "$season",
                    year: {$top: {output: "$year", sortBy: {year: -1}}},
                    kind: {$top: {output: "$kind", sortBy: {year: -1}}},
                },
            },
        ],
        index: {season: 1, year: 1},
    },
    // The change from SERVER-90017, available in v8.1+, treats $bottomN, $topN with N = 1 as
    // $bottom and $top.
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {
                $group:
                    {_id: "$season", year: {$bottomN: {n: 1, output: "$year", sortBy: {year: 1}}}}
            },
        ],
        index: {season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {
                $group:
                    {_id: "$season", year: {$bottomN: {n: 1, output: "$year", sortBy: {year: -1}}}}
            },
        ],
        index: {season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {$group: {_id: "$season", year: {$topN: {n: 1, output: "$year", sortBy: {year: 1}}}}},
        ],
        index: {season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {$group: {_id: "$season", year: {$topN: {n: 1, output: "$year", sortBy: {year: -1}}}}},
        ],
        index: {season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {kind: 1, season: 1, year: 1}},
            {
                $group:
                    {_id: "$kind", year: {$bottom: {output: "$year", sortBy: {season: 1, year: 1}}}}
            },
        ],
        index: {kind: 1, season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {kind: 1, season: 1, year: 1}},
            {
                $group: {
                    _id: "$kind",
                    year: {$bottom: {output: "$year", sortBy: {season: -1, year: -1}}}
                }
            },
        ],
        index: {kind: 1, season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {kind: 1, season: 1, year: 1}},
            {$group: {_id: "$kind", year: {$top: {output: "$year", sortBy: {season: 1, year: 1}}}}},
        ],
        index: {kind: 1, season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {kind: 1, season: 1, year: 1}},
            {
                $group:
                    {_id: "$kind", year: {$top: {output: "$year", sortBy: {season: -1, year: -1}}}}
            },
        ],
        index: {kind: 1, season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {kind: -1, season: -1, year: -1}},
            {
                $group:
                    {_id: "$kind", year: {$bottom: {output: "$year", sortBy: {season: 1, year: 1}}}}
            },
        ],
        index: {kind: 1, season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {kind: -1, season: -1, year: -1}},
            {
                $group: {
                    _id: "$kind",
                    year: {$bottom: {output: "$year", sortBy: {season: -1, year: -1}}}
                }
            },
        ],
        index: {kind: 1, season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {kind: -1, season: -1, year: -1}},
            {$group: {_id: "$kind", year: {$top: {output: "$year", sortBy: {season: 1, year: 1}}}}},
        ],
        index: {kind: 1, season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {kind: -1, season: -1, year: -1}},
            {
                $group:
                    {_id: "$kind", year: {$top: {output: "$year", sortBy: {season: -1, year: -1}}}}
            },
        ],
        index: {kind: 1, season: 1, year: 1},
    },
];

const indexScanTestCases = [
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {
                $group: {
                    _id: "$season",
                    year: {$bottom: {output: "$year", sortBy: {season: 1, year: -1}}}
                }
            },
        ],
        index: {season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {
                $group:
                    {_id: "$season", year: {$top: {output: "$year", sortBy: {season: 1, year: -1}}}}
            },
        ],
        index: {season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {kind: 1, season: 1, year: 1}},
            {$group: {_id: "$kind", year: {$bottom: {output: "$year", sortBy: {year: 1}}}}},
        ],
        index: {kind: 1, season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {kind: 1, season: 1, year: 1}},
            {$group: {_id: "$kind", year: {$bottom: {output: "$year", sortBy: {year: -1}}}}},
        ],
        index: {kind: 1, season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {kind: 1, season: 1, year: 1}},
            {$group: {_id: "$kind", year: {$top: {output: "$year", sortBy: {year: 1}}}}},
        ],
        index: {kind: 1, season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {kind: 1, season: 1, year: 1}},
            {$group: {_id: "$kind", year: {$top: {output: "$year", sortBy: {year: -1}}}}},
        ],
        index: {kind: 1, season: 1, year: 1},
    },
    // Mixed accumulators
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {
                $group: {
                    _id: "$season",
                    year: {$bottom: {output: "$year", sortBy: {year: 1}}},
                    kind: {$top: {output: "$kind", sortBy: {year: 1}}},
                },
            },
        ],
        index: {season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {
                $group: {
                    _id: "$season",
                    year: {$bottom: {output: "$year", sortBy: {year: -1}}},
                    kind: {$top: {output: "$kind", sortBy: {year: -1}}},
                },
            },
        ],
        index: {season: 1, year: 1},
    },
    // Mixed sortings in accumulators
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {
                $group: {
                    _id: "$season",
                    year: {$bottom: {output: "$year", sortBy: {year: -1}}},
                    kind: {$bottom: {output: "$kind", sortBy: {year: 1}}},
                },
            },
        ],
        index: {season: 1, year: 1},
    },
    {
        pipeline: [
            {$sort: {season: 1, year: 1}},
            {
                $group: {
                    _id: "$season",
                    year: {$top: {output: "$year", sortBy: {year: -1}}},
                    kind: {$top: {output: "$kind", sortBy: {year: 1}}},
                },
            },
        ],
        index: {season: 1, year: 1},
    },
];

function runTests(db, isDistinctScanOptimizationEnabled) {
    const coll = db[jsTest.name()];
    coll.drop();
    assert.commandWorked(coll.insertMany(docs));

    function assertPipeline(pipeline, validateExplain) {
        // Collect the expected output which is produced by the same query on the collection without
        // indexes, which guarantees that the tested optimizations are not made.
        assert.commandWorked(coll.dropIndexes());
        const expectedOutput = coll.aggregate(pipeline).toArray();

        assert.commandWorked(coll.createIndexes(indexes));

        const actualOutput = coll.aggregate(pipeline).toArray();
        const explain = coll.explain().aggregate(pipeline);

        assert.sameMembers(expectedOutput, actualOutput, explain);
        validateExplain(explain);
    }

    function assertDistinctScan(pipeline, expectedIndex) {
        const validateExplain = (explain) => assertPlanUsesDistinctScan(db, explain, expectedIndex);
        assertPipeline(pipeline, validateExplain);
    }

    function assertIndexScan(pipeline, expectedIndex) {
        const validateExplain = (explain) => assertPlanUsesIndexScan(explain, expectedIndex);
        assertPipeline(pipeline, validateExplain);
    }

    for (const testCase of alwaysEnabledDistictScanTestCases) {
        assertDistinctScan(testCase.pipeline, testCase.index);
    }

    for (const testCase of distictScanTestCases) {
        // TODO SERVER-113145: Some distinct scan optimizations are suppressed when shard filtering
        // feature is enabled.
        if (isDistinctScanOptimizationEnabled) {
            assertDistinctScan(testCase.pipeline, testCase.index);
        } else {
            assertIndexScan(testCase.pipeline, testCase.index);
        }
    }

    for (const testCase of indexScanTestCases) {
        assertIndexScan(testCase.pipeline, testCase.index);
    }
}

function runMongodAndTest(featureFlagShardFilteringDistinctScan) {
    const mongodOptions = {
        setParameter: {featureFlagShardFilteringDistinctScan},
    };
    const conn = MongoRunner.runMongod(mongodOptions);
    try {
        const db = conn.getDB(`${jsTest.name()}_db`);
        const isDistinctScanOptimizationEnabled = !featureFlagShardFilteringDistinctScan;
        runTests(db, isDistinctScanOptimizationEnabled);
    } finally {
        MongoRunner.stopMongod(conn);
    }
}

runMongodAndTest(/*featureFlagShardFilteringDistinctScan*/ false);
runMongodAndTest(/*featureFlagShardFilteringDistinctScan*/ true);
