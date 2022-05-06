/**
 * Testing of just the query layer's integration for columnar index.
 * @tags: [
 *   # columnstore indexes are new in 6.1.
 *   requires_fcv_61,
 *   # We could potentially need to resume an index build in the event of a stepdown, which is not
 *   # yet implemented.
 *   does_not_support_stepdowns,
 *   # Columnstore indexes are incompatible with clustered collections.
 *   incompatible_with_clustered_collection,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/analyze_plan.js");
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const columnstoreEnabled = checkSBEEnabled(db, ["featureFlagColumnstoreIndexes"]);
if (!columnstoreEnabled) {
    jsTestLog("Skipping columnstore index validation test since the feature flag is not enabled.");
    return;
}

const coll = db.columnstore_index_correctness;
coll.drop();

const docs = [
    {},
    {"a": null},
    {"a": "scalar"},
    {"a": {}},
    {"a": {"x": 1, "b": "scalar"}},
    {
        "a": {
            "b": {

            }
        }
    },
    {
        "a": {
            "x": 1,
            "b": {

            }
        }
    },
    {"a": {"x": 1, "b": {"x": 1}}},
    {"a": {"b": {"c": "scalar"}}},
    {"a": {"b": {"c": null}}},
    {
        "a": {
            "b": {
                "c": [
                    [1, 2],
                    [{

                    }],
                    2
                ]
            }
        }
    },
    {"a": {"x": 1, "b": {"x": 1, "c": ["scalar"]}}},
    {"a": {"x": 1, "b": {"c": {"x": 1}}}},
    {"a": {"b": []}},
    {"a": {"b": [null]}},
    {"a": {"b": ["scalar"]}},
    {"a": {"b": [[]]}},
    {
        "a": {
            "b": [
                1,
                {

                },
                2
            ]
        }
    },
    {
        "a": {
            "b": [
                [1, 2],
                [{

                }],
                2
            ]
        }
    },
    {
        "a": {
            "x": 1,
            "b": [
                [1, 2],
                [{

                }],
                2
            ]
        }
    },
    {"a": {"b": [{"c": "scalar"}]}},
    {"a": {"b": [{"c": "scalar"}, {"c": "scalar2"}]}},
    {
        "a": {
            "b": [{
                "c": [
                    [1, 2],
                    [{

                    }],
                    2
                ]
            }]
        }
    },
    {"a": {"b": [1, {"c": "scalar"}, 2]}},
    {
        "a": {
            "b": [
                1,
                {
                    "c": [
                        [1, 2],
                        [{

                        }],
                        2
                    ]
                },
                2
            ]
        }
    },
    {
        "a": {
            "x": 1,
            "b": [
                1,
                {
                    "c": [
                        [1, 2],
                        [{

                        }],
                        2
                    ]
                },
                2
            ]
        }
    },
    {"a": {"b": [[1, 2], [{"c": "scalar"}], 2]}},
    {
        "a": {
            "b": [
                [1, 2],
                [{
                    "c": [
                        [1, 2],
                        [{

                        }],
                        2
                    ]
                }],
                2
            ]
        }
    },
    {
        "a": {
            "x": 1,
            "b": [
                [1, 2],
                [{
                    "c": [
                        [1, 2],
                        [{

                        }],
                        2
                    ]
                }],
                2
            ]
        }
    },
    {"a": []},
    {"a": [null]},
    {"a": ["scalar"]},
    {"a": [[]]},
    {
        "a": [{

        }]
    },
    {
        "a": [
            1,
            {

            },
            2
        ]
    },
    {
        "a": [
            [1, 2],
            [{

            }],
            2
        ]
    },
    {"a": [{"b": "scalar"}]},
    {"a": [{"b": null}]},
    {"a": [1, {"b": "scalar"}, 2]},
    {"a": [1, {"b": []}, 2]},
    {"a": [1, {"b": [null]}, 2]},
    {"a": [1, {"b": ["scalar"]}, 2]},
    {"a": [1, {"b": [[]]}, 2]},
    {"a": [{"b": []}]},
    {"a": [{"b": ["scalar"]}]},
    {"a": [{"b": [[]]}]},
    {
        "a": [{
            "b": {

            }
        }]
    },
    {"a": [{"b": {"c": "scalar"}}]},
    {
        "a": [{
            "b": {
                "c": [
                    [1, 2],
                    [{

                    }],
                    2
                ]
            }
        }]
    },
    {"a": [{"b": {"x": 1}}]},
    {"a": [{"b": {"x": 1, "c": "scalar"}}]},
    {"a": [{"b": [{"c": "scalar"}]}]},
    {"a": [{"b": [{"c": ["scalar"]}]}]},
    {"a": [{"b": [1, {"c": ["scalar"]}, 2]}]},
    {
        "a": [{
            "b": [{

            }]
        }]
    },
    {
        "a": [{
            "b": [
                [1, 2],
                [{

                }],
                2
            ]
        }]
    },
    {"a": [{"b": [[1, 2], [{"c": "scalar"}], 2]}]},
    {"a": [{"b": [[1, 2], [{"c": ["scalar"]}], 2]}]},
    {
        "a": [
            1,
            {
                "b": {

                }
            },
            2
        ]
    },
    {"a": [1, {"b": {"c": "scalar"}}, 2]},
    {"a": [1, {"b": {"c": {"x": 1}}}, 2]},
    {
        "a": [
            1,
            {
                "b": {
                    "c": [
                        1,
                        {

                        },
                        2
                    ]
                }
            },
            2
        ]
    },
    {"a": [1, {"b": {"x": 1}}, 2]},
    {"a": [1, {"b": {"x": 1, "c": "scalar"}}, 2]},
    {"a": [1, {"b": {"x": 1, "c": [[]]}}, 2]},
    {
        "a": [
            1,
            {
                "b": {
                    "x": 1,
                    "c": [
                        1,
                        {

                        },
                        2
                    ]
                }
            },
            2
        ]
    },
    {
        "a": [
            1,
            {
                "b": [{

                }]
            },
            2
        ]
    },
    {"a": [1, {"b": [{"c": "scalar"}]}, 2]},
    {"a": [1, {"b": [{"c": {"x": 1}}]}, 2]},
    {
        "a": [
            1,
            {
                "b": [{
                    "c": [
                        1,
                        {

                        },
                        2
                    ]
                }]
            },
            2
        ]
    },
    {
        "a": [
            1,
            {
                "b": [
                    1,
                    {

                    },
                    2
                ]
            },
            2
        ]
    },
    {"a": [1, {"b": [1, {"c": null}, 2]}, 2]},
    {"a": [1, {"b": [1, {"c": "scalar"}, 2]}, 2]},
    {
        "a": [
            1,
            {
                "b": [
                    1,
                    {
                        "c": [
                            1,
                            {

                            },
                            2
                        ]
                    },
                    2
                ]
            },
            2
        ]
    },
    {
        "a": [
            1,
            {
                "b": [
                    [1, 2],
                    [{

                    }],
                    2
                ]
            },
            2
        ]
    },
    {"a": [1, {"b": [[1, 2], [{"c": "scalar"}], 2]}, 2]},
    {
        "a": [
            1,
            {
                "b": [
                    [1, 2],
                    [{
                        "c": [
                            1,
                            {

                            },
                            2
                        ]
                    }],
                    2
                ]
            },
            2
        ]
    },
    {"a": [[1, 2], [{"b": "scalar"}], 2]},
    {"a": [[1, 2], [{"b": {"x": 1, "c": "scalar"}}], 2]},
    {
        "a": [
            [1, 2],
            [{
                "b": {
                    "x": 1,
                    "c": [
                        1,
                        {

                        },
                        2
                    ]
                }
            }],
            2
        ]
    },
    {"a": [[1, 2], [{"b": []}], 2]},
    {"a": [[1, 2], [{"b": [1, {"c": "scalar"}, 2]}], 2]},
    {"a": [[1, 2], [{"b": [[1, 2], [{"c": "scalar"}], 2]}], 2]},
    {
        "a": [
            [1, 2],
            [{
                "b": [
                    [1, 2],
                    [{
                        "c": [
                            [1, 2],
                            [{

                            }],
                            2
                        ]
                    }],
                    2
                ]
            }],
            2
        ]
    },
    {
        "a": [{
            "b": [
                {"c": 1},
                {

                }
            ]
        }]
    },
    {"a": [{"b": [{"c": 1}, {"d": 1}]}]},
    {
        "a": [
            {"b": {"c": 1}},
            {
                "b": {

                }
            }
        ]
    },
    {"a": [{"b": {"c": 1}}, {"b": {"d": 1}}]},
    {
        "a": [
            {"b": {"c": 1}},
            {

            }
        ]
    },
    {"a": [{"b": {"c": 1}}, {"b": null}]},
    {"a": [{"b": {"c": 1}}, {"b": []}]},
    {"a": [{"b": []}, {"b": []}]},

    {a: {b: [{c: [1, 2]}]}},
    {a: {b: {c: [1, 2]}}},
    {a: [[1, 2], [{b: [[1, 2], [{c: [[1, 2], [{}], 2]}], 2]}], 2]},
];

let docNum = 0;
let bulk = coll.initializeUnorderedBulkOp();
for (let doc of docs) {
    // Intentionally not using _id as the unique identifier, to avoid getting IDHACK plans when we
    // query by it.
    let numObj = {num: docNum++};
    let insertObj = {};
    Object.assign(insertObj, numObj, doc);
    if (docNum % 2 == 0) {
        insertObj.optionalField = "foo";
    }
    bulk.insert(insertObj);
}
bulk.execute();

assert.commandWorked(coll.createIndex({"$**": "columnstore"}));
const kProjection = {
    _id: 0,
    "a.b.c": 1,
    num: 1,
    optionalField: 1
};

// Run an explain.
const explain = coll.find({}, kProjection).explain();
assert(planHasStage(db, explain, "COLUMN_SCAN"), explain);

// Run a query getting all of the results using the column index.
let results = coll.find({}, kProjection).toArray();
assert.gt(results.length, 0);

for (let res of results) {
    const trueResult = coll.find({num: res.num}, kProjection).hint({$natural: 1}).toArray()[0];
    const originalDoc = coll.findOne({num: res.num});
    assert.eq(res, trueResult, originalDoc);
}
})();
