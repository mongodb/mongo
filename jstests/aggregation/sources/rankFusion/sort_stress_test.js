/**
 * Stress tests the cases where a sort order is interesting, and so the rank computation needs to be
 * careful. For example, when a sub-pipeline specifies a $sort.
 * @tags: [ featureFlagRankFusionBasic, featureFlagRankFusionFull, requires_fcv_81 ]
 */
import {orderedArrayEq} from "jstests/aggregation/extras/utils.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

const allDocs = [
    {
        _id: 1,
        dish_name: "Cardamom bun",
        flavor: "Some spice, but not a heat more of a floral note",
        n_per_serving: 4,
        tasty: true,
    },
    {
        _id: 2,
        dish_name: "truffle pancakes",
        flavor: "It's giving stinky socks. Yuck. Chef's spit.",
        n_per_serving: 2,
        tasty: false,
    },
    {
        _id: 3,
        dish_name: "herby thyme marinade",
        flavor: "Like a lemony thing going on. Quite intriguing. I think it'd be good on potatoes",
        n_per_serving: 3,
        tasty: true,
    },
    {
        _id: 4,
        dish_name: "maple bacon acorn squash",
        flavor: "A little too sweet for me, but the flavor is nice",
        n_per_serving: 3,
        tasty: true,
    },
    {
        _id: 5,
        dish_name: "Zaxby's fish sticks",
        flavor: "A sad attempt. It's gross. Please do better next time.",
        n_per_serving: 4,
        tasty: false,
    },
];
assert.commandWorked(coll.insertMany(allDocs));

function testRankFusion({pipeline, expectedResults}) {
    let results = coll.aggregate(pipeline).toArray();
    assert(orderedArrayEq(results, expectedResults));
}

function withAndWithoutIndex({index, assertFn}) {
    assertFn();
    assert.commandWorked(coll.createIndex(index));
    assertFn();
    assert.commandWorked(db.runCommand({dropIndexes: coll.getName(), index}));
}

{
    // Basic test of one pipeline, to demonstrate.
    testRankFusion({
        pipeline: [
            {
                $rankFusion: {
                    input: {
                        pipelines: {
                            tasty: [
                                {$match: {tasty: true}},
                                {$sort: {n_per_serving: -1, _id: 1}},
                            ],
                        },
                    },
                },
            },
            // Just to trim the test down a bit.
            {$unset: ["dish_name", "flavor"]},
            {$limit: 2}
        ],
        expectedResults: [
            {
                _id: 1,
                n_per_serving: 4,
                tasty: true,
            },
            {
                _id: 3,
                n_per_serving: 3,
                tasty: true,
            }
        ]
    });
}

{
    // Now the same one, but with scoreDetails.
    testRankFusion({
        pipeline: [
            {
                $rankFusion: {
                    input: {
                        pipelines: {
                            tasty: [
                                {$match: {tasty: true}},
                                {$sort: {n_per_serving: -1, _id: 1}},
                            ],
                        },
                    },
                    scoreDetails: true,
                },
            },
            {$set: {details: {$meta: "scoreDetails"}}},
            {$unset: ["dish_name", "flavor"]},
            {$limit: 2}
        ],
        expectedResults: [
            {
                _id: 1,
                n_per_serving: 4,
                tasty: true,
                details: {
                    value: 0.01639344262295082,
                    details: {tasty: {rank: 1, details: "Not Calculated"}}
                }
            },
            {
                _id: 3,
                n_per_serving: 3,
                tasty: true,
                details: {
                    value: 0.016129032258064516,
                    details: {tasty: {rank: 2, details: "Not Calculated"}}
                }
            }
        ]
    });
}

{
    // Now test that the latest sort is the one obeyed - since we have a dependency on the sort key
    // which we need to be careful to get right.
    const actualExpectedSortSpec = {n_per_serving: 1, _id: 1};
    const actualExpectedSort = {$sort: actualExpectedSortSpec};
    function testWithSortVariation(sortStages) {
        const pipeline = [
            {
                $rankFusion: {
                    input: {
                        pipelines: {
                            tasty: [{$match: {tasty: true}}, ...sortStages],
                        },
                    },
                    scoreDetails: true,
                },
            },
            {$set: {details: {$meta: "scoreDetails"}}},
            // Just to trim the test down a bit.
            {$unset: ["dish_name", "flavor"]},
            {$limit: 2}
        ];
        const expectedResults = [
            {

                _id: 3,
                n_per_serving: 3,
                tasty: true,
                details: {
                    value: 0.01639344262295082,
                    details: {tasty: {rank: 1, details: "Not Calculated"}}
                }
            },
            {
                _id: 4,
                n_per_serving: 3,
                tasty: true,
                details: {
                    value: 0.016129032258064516,
                    details: {tasty: {rank: 2, details: "Not Calculated"}}
                }
            }
        ];
        testRankFusion({pipeline, expectedResults});
    }
    // This sort should never actually be used. It'll change the expected results. We test it
    // here to make sure that redundant sorts and optimizations don't throw us off.
    withAndWithoutIndex({
        index: actualExpectedSortSpec,
        assertFn: () => {
            const irrelevantSort = {$sort: {n_per_serving: -1, _id: 1}};
            testWithSortVariation([
                actualExpectedSort,
            ]);
            testWithSortVariation([irrelevantSort, actualExpectedSort]);
            testWithSortVariation(
                [irrelevantSort, {$_internalInhibitOptimization: {}}, actualExpectedSort]);
            testWithSortVariation([{$_internalInhibitOptimization: {}}, actualExpectedSort]);
        }
    });
}

{
    // A couple more variations of interesting sorts.
    const tripleCompoundSort = {tasty: -1, n_per_serving: -1, _id: 1};
    withAndWithoutIndex({
        index: tripleCompoundSort,
        assertFn: () => testRankFusion({
            pipeline: [
                {
                    $rankFusion: {
                        input: {
                            pipelines: {
                                everything_sorted: [
                                    {$sort: tripleCompoundSort},
                                ]
                            }
                        }
                    }
                },
                {$project: {tasty: 1, n_per_serving: 1}}
            ],
            expectedResults: [
                {_id: 1, n_per_serving: 4, tasty: true},
                {_id: 3, n_per_serving: 3, tasty: true},
                {_id: 4, n_per_serving: 3, tasty: true},
                {_id: 5, n_per_serving: 4, tasty: false},
                {_id: 2, n_per_serving: 2, tasty: false},
            ],
        })
    });
}

{
    // Now test multiple inputs.
    const rankFusionSpec = {
        input: {
            pipelines: {
                tasty: [
                    {$match: {tasty: true}},
                    {$sort: {n_per_serving: -1, _id: 1}},
                ],
                has_a_but: [
                    {$match: {flavor: /but/}},
                    {$sort: {n_per_serving: 1, _id: -1}},
                ],
            },
        },
    };
    let expectedResults = [
        {
            _id: 1,
            flavor: "Some spice, but not a heat more of a floral note",
            n_per_serving: 4,
            tasty: true,
        },
        {
            _id: 4,
            flavor: "A little too sweet for me, but the flavor is nice",
            n_per_serving: 3,
            tasty: true,
        },
        {
            _id: 3,
            flavor:
                "Like a lemony thing going on. Quite intriguing. I think it'd be good on potatoes",
            n_per_serving: 3,
            tasty: true,
        }
    ];
    testRankFusion({
        pipeline: [{$rankFusion: rankFusionSpec}, {$unset: ["dish_name"]}, {$limit: 3}],
        expectedResults
    });
    // Add one more pipeline which shouldn't influence the results at all.
    rankFusionSpec.input.pipelines.everything = [{$sort: {no_such_field: 1 /* all ties */}}];
    testRankFusion({
        pipeline: [{$rankFusion: rankFusionSpec}, {$unset: ["dish_name"]}, {$limit: 3}],
        expectedResults
    });
    rankFusionSpec.scoreDetails = true;
    testRankFusion({
        pipeline: [
            {$rankFusion: rankFusionSpec},
            {$project: {n_per_serving: 1, flavor: 1, tasty: 1, details: {$meta: "scoreDetails"}}},
            {$limit: 3}
        ],
        expectedResults: [
            {
                _id: 1,
                n_per_serving: 4,
                flavor: "Some spice, but not a heat more of a floral note",
                tasty: true,
                details: {
                    value: 0.048915917503966164,
                    details: {
                        everything: {rank: 1, details: "Not Calculated"},
                        has_a_but: {rank: 2, details: "Not Calculated"},
                        tasty: {rank: 1, details: "Not Calculated"}
                    }
                }
            },
            {
                _id: 4,
                n_per_serving: 3,
                flavor: "A little too sweet for me, but the flavor is nice",
                tasty: true,
                details: {
                    value: 0.04865990111891751,
                    details: {
                        everything: {rank: 1, details: "Not Calculated"},
                        has_a_but: {rank: 1, details: "Not Calculated"},
                        tasty: {rank: 3, details: "Not Calculated"}
                    }
                }
            },
            {
                _id: 3,
                n_per_serving: 3,
                flavor:
                    "Like a lemony thing going on. Quite intriguing. I think it'd be good on potatoes",
                tasty: true,
                details: {
                    value: 0.03252247488101534,
                    details: {
                        everything: {rank: 1, details: "Not Calculated"},
                        has_a_but: {rank: NumberLong(0)},
                        tasty: {rank: 2, details: "Not Calculated"}
                    }
                }
            }
        ]
    });
}
