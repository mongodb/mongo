// Tests for the $lookup stage with a sub-pipeline.

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertErrorCode and anyEq.
load("jstests/libs/discover_topology.js");    // For findNonConfigNodes.

const testName = "lookup_subpipeline";

const coll = db.lookUp;
const from = db.from;
const thirdColl = db.thirdColl;
const fourthColl = db.fourthColl;

// Helper for testing that pipeline returns correct set of results.
function testPipeline(pipeline, expectedResult, collection) {
    assert(anyEq(collection.aggregate(pipeline).toArray(), expectedResult));
}

//
// Pipeline syntax using 'let' variables.
//
coll.drop();
assert.commandWorked(coll.insert({_id: 1, x: 1}));
assert.commandWorked(coll.insert({_id: 2, x: 2}));
assert.commandWorked(coll.insert({_id: 3, x: 3}));

from.drop();
assert.commandWorked(from.insert({_id: 1}));
assert.commandWorked(from.insert({_id: 2}));
assert.commandWorked(from.insert({_id: 3}));

// Basic non-equi theta join via $project.
let pipeline = [
        {
          $lookup: {
              let : {var1: "$_id"},
              pipeline: [
                  {$project: {isMatch: {$gt: ["$$var1", "$_id"]}}},
                  {$match: {isMatch: true}},
                  {$project: {isMatch: 0}}
              ],
              from: "from",
              as: "c",
          }
        },
    ];

let expectedResults = [
    {"_id": 1, x: 1, "c": []},
    {"_id": 2, x: 2, "c": [{"_id": 1}]},
    {
        "_id": 3,
        x: 3,
        "c": [
            {"_id": 1},
            {
                "_id": 2,
            }
        ]
    }
];
testPipeline(pipeline, expectedResults, coll);
// Basic non-equi theta join via $match.
pipeline = [
        {
          $lookup: {
              let : {var1: "$_id"},
              pipeline: [
                  {$match: {$expr: {$lt: ["$_id", "$$var1"]}}},
              ],
              from: "from",
              as: "c",
          }
        },
    ];

expectedResults = [
    {"_id": 1, x: 1, "c": []},
    {"_id": 2, x: 2, "c": [{"_id": 1}]},
    {
        "_id": 3,
        x: 3,
        "c": [
            {"_id": 1},
            {
                "_id": 2,
            }
        ]
    }
];
testPipeline(pipeline, expectedResults, coll);

// Basic non-equi theta join via $group.
pipeline =
    [{$lookup: {from: "from", pipeline: [{$group: {_id: "$_id", avg: {$avg: "$_id"}}}], as: "c"}}];

expectedResults = [
    {"_id": 1, x: 1, "c": [{"_id": 1, "avg": 1}, {"_id": 2, "avg": 2}, {"_id": 3, "avg": 3}]},
    {"_id": 2, x: 2, "c": [{"_id": 1, "avg": 1}, {"_id": 2, "avg": 2}, {"_id": 3, "avg": 3}]},
    {"_id": 3, x: 3, "c": [{"_id": 1, "avg": 1}, {"_id": 2, "avg": 2}, {"_id": 3, "avg": 3}]},
];
testPipeline(pipeline, expectedResults, coll);

// Multi-level join using $match.
pipeline = [
        {
          $lookup: {
              let : {var1: "$_id"},
              pipeline: [
                  {$match: {$expr: {$eq: ["$_id", "$$var1"]}}},
                  {
                    $lookup: {
                        let : {var2: "$_id"},
                        pipeline: [{$match: {$expr: {$gt: ["$_id", "$$var2"]}}}, {$sort: {_id: 1}}],
                        from: "from",
                        as: "d"
                    }
                  },
              ],
              from: "from",
              as: "c",
          }
        },
    ];

expectedResults = [
    {"_id": 1, "x": 1, "c": [{"_id": 1, "d": [{"_id": 2}, {"_id": 3}]}]},
    {"_id": 2, "x": 2, "c": [{"_id": 2, "d": [{"_id": 3}]}]},
    {"_id": 3, "x": 3, "c": [{"_id": 3, "d": []}]}
];
testPipeline(pipeline, expectedResults, coll);

// Equijoin with $match that can't be delegated to the query subsystem.
pipeline = [
        {
          $lookup: {
              let : {var1: "$x"},
              pipeline: [
                  {$addFields: {newField: 2}},
                  {$match: {$expr: {$eq: ["$newField", "$$var1"]}}},
                  {$project: {newField: 0}},
                  {$sort: {_id: 1}}
              ],
              from: "from",
              as: "c",
          }
        },
    ];

expectedResults = [
    {"_id": 1, "x": 1, "c": []},
    {"_id": 2, "x": 2, "c": [{"_id": 1}, {"_id": 2}, {"_id": 3}]},
    {"_id": 3, "x": 3, "c": []}
];
testPipeline(pipeline, expectedResults, coll);

// Multiple variables.
pipeline = [
        {
          $lookup: {
              let : {var1: "$_id", var2: "$x"},
              pipeline: [
                  {
                    $project: {
                        isMatch: {$gt: ["$$var1", "$_id"]},
                        var2Times2: {$multiply: [2, "$$var2"]}
                    }
                  },
                  {$match: {isMatch: true}},
                  {$project: {isMatch: 0}}
              ],
              from: "from",
              as: "c",
          },
        },
        {$project: {x: 1, c: 1}}
    ];

expectedResults = [
    {"_id": 1, x: 1, "c": []},
    {"_id": 2, x: 2, "c": [{"_id": 1, var2Times2: 4}]},
    {"_id": 3, x: 3, "c": [{"_id": 1, var2Times2: 6}, {"_id": 2, var2Times2: 6}]}
];
testPipeline(pipeline, expectedResults, coll);

// Let var as complex expression object.
pipeline = [
        {
          $lookup: {
              let : {var1: {$mod: ["$x", 3]}},
              pipeline: [
                  {$project: {var1Mod3TimesForeignId: {$multiply: ["$$var1", "$_id"]}}},
              ],
              from: "from",
              as: "c",
          }
        },
    ];

expectedResults = [
    {
        "_id": 1,
        x: 1,
        "c": [
            {_id: 1, var1Mod3TimesForeignId: 1},
            {_id: 2, var1Mod3TimesForeignId: 2},
            {_id: 3, var1Mod3TimesForeignId: 3}
        ]
    },
    {
        "_id": 2,
        x: 2,
        "c": [
            {_id: 1, var1Mod3TimesForeignId: 2},
            {_id: 2, var1Mod3TimesForeignId: 4},
            {_id: 3, var1Mod3TimesForeignId: 6}
        ]
    },
    {
        "_id": 3,
        x: 3,
        "c": [
            {_id: 1, var1Mod3TimesForeignId: 0},
            {_id: 2, var1Mod3TimesForeignId: 0},
            {_id: 3, var1Mod3TimesForeignId: 0}
        ]
    }
];
testPipeline(pipeline, expectedResults, coll);

// 'let' defined variables are available to all nested sub-pipelines.
pipeline = [
        {$match: {_id: 1}},
        {
          $lookup: {
              let : {var1: "ABC", var2: "123"},
              pipeline: [
                  {$match: {_id: 1}},
                  {
                    $lookup: {
                        pipeline: [
                            {$match: {_id: 2}},
                            {$addFields: {letVar1: "$$var1"}},
                            {
                              $lookup: {
                                  let : {var3: "XYZ"},
                                  pipeline: [{
                                      $addFields: {
                                          mergedLetVars:
                                              {$concat: ["$$var1", "$$var2", "$$var3"]}
                                      }
                                  }],
                                  from: "from",
                                  as: "join3"
                              }
                            },
                        ],
                        from: "from",
                        as: "join2"
                    }
                  },
              ],
              from: "from",
              as: "join1",
          }
        }
    ];

expectedResults = [{
    "_id": 1,
    "x": 1,
    "join1": [{
        "_id": 1,
        "join2": [{
            "_id": 2,
            "letVar1": "ABC",
            "join3": [
                {"_id": 1, "mergedLetVars": "ABC123XYZ"},
                {"_id": 2, "mergedLetVars": "ABC123XYZ"},
                {"_id": 3, "mergedLetVars": "ABC123XYZ"}
            ]
        }]
    }]
}];
testPipeline(pipeline, expectedResults, coll);

// 'let' variable shadowed by foreign pipeline variable.
pipeline = [
        {$match: {_id: 2}},
        {
          $lookup: {
              let : {var1: "$_id"},
              pipeline: [
                  {
                    $project: {
                        shadowedVar: {$let: {vars: {var1: "abc"}, in : "$$var1"}},
                        originalVar: "$$var1"
                    }
                  },
                  {
                    $lookup: {
                        pipeline: [{
                            $project: {
                                shadowedVar: {$let: {vars: {var1: "xyz"}, in : "$$var1"}},
                                originalVar: "$$var1"
                            }
                        }],
                        from: "from",
                        as: "d"
                    }
                  }
              ],
              from: "from",
              as: "c",
          }
        }
    ];

expectedResults = [{
    "_id": 2,
    "x": 2,
    "c": [
        {
            "_id": 1,
            "shadowedVar": "abc",
            "originalVar": 2,
            "d": [
                {"_id": 1, "shadowedVar": "xyz", "originalVar": 2},
                {"_id": 2, "shadowedVar": "xyz", "originalVar": 2},
                {"_id": 3, "shadowedVar": "xyz", "originalVar": 2}
            ]
        },
        {
            "_id": 2,
            "shadowedVar": "abc",
            "originalVar": 2,
            "d": [
                {"_id": 1, "shadowedVar": "xyz", "originalVar": 2},
                {"_id": 2, "shadowedVar": "xyz", "originalVar": 2},
                {"_id": 3, "shadowedVar": "xyz", "originalVar": 2}
            ]
        },
        {
            "_id": 3,
            "shadowedVar": "abc",
            "originalVar": 2,
            "d": [
                {"_id": 1, "shadowedVar": "xyz", "originalVar": 2},
                {"_id": 2, "shadowedVar": "xyz", "originalVar": 2},
                {"_id": 3, "shadowedVar": "xyz", "originalVar": 2}
            ]
        }
    ]
}];
testPipeline(pipeline, expectedResults, coll);

// Use of undefined variable fails.
assertErrorCode(coll,
                    [{
                       $lookup: {
                           from: "from",
                           as: "as",
                           let : {var1: "$x"},
                           pipeline: [{$project: {myVar: "$$nonExistent"}}]
                       }
                    }],
                    17276);
assertErrorCode(
    coll,
    [{$lookup: {let : {var1: 1, var2: "$$var1"}, pipeline: [], from: "from", as: "as"}}],
    17276);
assertErrorCode(coll,
                    [{
                       $lookup: {
                           let : {
                               var1: {$let: {vars: {var1: 2}, in : "$$var1"}},
                               var2: {$let: {vars: {var1: 4}, in : "$$var2"}},
                           },
                           pipeline: [],
                           from: "from",
                           as: "as"
                       }
                    }],
                    17276);

// The dotted path offset of a non-object variable is equivalent referencing an undefined
// field.
pipeline = [
        {
          $lookup: {
              let : {var1: "$x"},
              pipeline: [
                  {
                    $match: {
                        $expr:
                            {$eq: ["FIELD-IS-NULL", {$ifNull: ["$$var1.y.z", "FIELD-IS-NULL"]}]}
                    }
                  },
              ],
              from: "from",
              as: "as",
          }
        },
        {$project: {_id: 0}},
        {$sort: {x: 1}}
    ];

expectedResults = [
    {"x": 1, "as": [{"_id": 1}, {"_id": 2}, {"_id": 3}]},
    {"x": 2, "as": [{"_id": 1}, {"_id": 2}, {"_id": 3}]},
    {"x": 3, "as": [{"_id": 1}, {"_id": 2}, {"_id": 3}]}
];
testPipeline(pipeline, expectedResults, coll);

// Test that empty subpipeline optimizes correctly with following $match.
pipeline = [
    {$match: {_id: 1}},
    {$lookup: {from: "from", pipeline: [], as: "as"}},
    {$unwind: "$as"},
    {$match: {"as._id": {$gt: 2}}}
];
expectedResults = [{_id: 1, x: 1, as: {_id: 3}}];
testPipeline(pipeline, expectedResults, coll);

// Comparison where a 'let' variable references an array.
coll.drop();
assert.commandWorked(coll.insert({x: [1, 2, 3]}));

pipeline = [
        {
          $lookup: {
              let : {var1: "$x"},
              pipeline: [
                  {$match: {$expr: {$eq: ["$$var1", [1, 2, 3]]}}},
              ],
              from: "from",
              as: "as",
          }
        },
        {$project: {_id: 0}}
    ];

expectedResults = [{"x": [1, 2, 3], "as": [{"_id": 1}, {"_id": 2}, {"_id": 3}]}];
testPipeline(pipeline, expectedResults, coll);

//
// Pipeline syntax with nested object.
//
coll.drop();
assert.commandWorked(coll.insert({x: {y: {z: 10}}}));

// Subfields of 'let' variables can be referenced via dotted path.
pipeline = [
        {
          $lookup: {
              let : {var1: "$x"},
              pipeline: [
                  {$project: {z: "$$var1.y.z"}},
              ],
              from: "from",
              as: "as",
          }
        },
        {$project: {_id: 0}}
    ];

expectedResults = [
    {"x": {"y": {"z": 10}}, "as": [{"_id": 1, "z": 10}, {"_id": 2, "z": 10}, {"_id": 3, "z": 10}]}
];
testPipeline(pipeline, expectedResults, coll);

// 'let' variable with dotted field path off of $$ROOT.
pipeline = [
        {
          $lookup: {
              let : {var1: "$$ROOT.x.y.z"},
              pipeline:
                  [{$match: {$expr: {$eq: ["$$var1", "$$ROOT.x.y.z"]}}}, {$project: {_id: 0}}],
              from: "lookUp",
              as: "as",
          }
        },
        {$project: {_id: 0}}
    ];

expectedResults = [{"x": {"y": {"z": 10}}, "as": [{"x": {"y": {"z": 10}}}]}];
testPipeline(pipeline, expectedResults, coll);

// 'let' variable with dotted field path off of $$CURRENT.
pipeline = [
        {
          $lookup: {
              let : {var1: "$$CURRENT.x.y.z"},
              pipeline:
                  [{$match: {$expr: {$eq: ["$$var1", "$$CURRENT.x.y.z"]}}}, {$project: {_id: 0}}],
              from: "lookUp",
              as: "as",
          }
        },
        {$project: {_id: 0}}
    ];

expectedResults = [{"x": {"y": {"z": 10}}, "as": [{"x": {"y": {"z": 10}}}]}];
testPipeline(pipeline, expectedResults, coll);

//
// Pipeline syntax with nested $lookup.
//
coll.drop();
assert.commandWorked(coll.insert({_id: 1, w: 1}));
assert.commandWorked(coll.insert({_id: 2, w: 2}));
assert.commandWorked(coll.insert({_id: 3, w: 3}));

from.drop();
assert.commandWorked(from.insert({_id: 1, x: 1}));
assert.commandWorked(from.insert({_id: 2, x: 2}));
assert.commandWorked(from.insert({_id: 3, x: 3}));

thirdColl.drop();
assert.commandWorked(thirdColl.insert({_id: 1, y: 1}));
assert.commandWorked(thirdColl.insert({_id: 2, y: 2}));
assert.commandWorked(thirdColl.insert({_id: 3, y: 3}));

fourthColl.drop();
assert.commandWorked(fourthColl.insert({_id: 1, z: 1}));
assert.commandWorked(fourthColl.insert({_id: 2, z: 2}));
assert.commandWorked(fourthColl.insert({_id: 3, z: 3}));

// Nested $lookup pipeline.
pipeline = [
        {$match: {_id: 1}},
        {
          $lookup: {
              pipeline: [
                  {$match: {_id: 2}},
                  {
                    $lookup: {
                        pipeline: [
                            {$match: {_id: 3}},
                            {
                              $lookup: {
                                  pipeline: [
                                      {$match: {_id: 1}},
                                  ],
                                  from: "fourthColl",
                                  as: "thirdLookup"
                              }
                            },
                        ],
                        from: "thirdColl",
                        as: "secondLookup"
                    }
                  },
              ],
              from: "from",
              as: "firstLookup",
          }
        }
    ];

expectedResults = [{
    "_id": 1,
    "w": 1,
    "firstLookup":
        [{"_id": 2, x: 2, "secondLookup": [{"_id": 3, y: 3, "thirdLookup": [{_id: 1, z: 1}]}]}]
}];
testPipeline(pipeline, expectedResults, coll);

//
// Error cases.
//

// 'pipeline' and 'let' must be of expected type.
assertErrorCode(coll, [{$lookup: {pipeline: 1, from: "from", as: "as"}}], ErrorCodes.TypeMismatch);
assertErrorCode(coll, [{$lookup: {pipeline: {}, from: "from", as: "as"}}], ErrorCodes.TypeMismatch);
assertErrorCode(
    coll, [{$lookup: {let : 1, pipeline: [], from: "from", as: "as"}}], ErrorCodes.FailedToParse);
assertErrorCode(
    coll, [{$lookup: {let : [], pipeline: [], from: "from", as: "as"}}], ErrorCodes.FailedToParse);
}());
