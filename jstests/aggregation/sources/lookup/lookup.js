// Basic $lookup regression tests.

load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

(function() {
    "use strict";

    // Used by testPipeline to sort result documents. All _ids must be primitives.
    function compareId(a, b) {
        if (a._id < b._id) {
            return -1;
        }
        if (a._id > b._id) {
            return 1;
        }
        return 0;
    }

    function generateNestedPipeline(foreignCollName, numLevels) {
        let pipeline = [{"$lookup": {pipeline: [], from: foreignCollName, as: "same"}}];

        for (let level = 1; level < numLevels; level++) {
            pipeline = [{"$lookup": {pipeline: pipeline, from: foreignCollName, as: "same"}}];
        }

        return pipeline;
    }

    // Helper for testing that pipeline returns correct set of results.
    function testPipeline(pipeline, expectedResult, collection) {
        assert.eq(collection.aggregate(pipeline).toArray().sort(compareId),
                  expectedResult.sort(compareId));
    }

    function runTest(coll, from, thirdColl, fourthColl) {
        var db = null;  // Using the db variable is banned in this function.

        assert.writeOK(coll.insert({_id: 0, a: 1}));
        assert.writeOK(coll.insert({_id: 1, a: null}));
        assert.writeOK(coll.insert({_id: 2}));

        assert.writeOK(from.insert({_id: 0, b: 1}));
        assert.writeOK(from.insert({_id: 1, b: null}));
        assert.writeOK(from.insert({_id: 2}));

        //
        // Basic functionality.
        //

        // "from" document added to "as" field if a == b, where nonexistent fields are treated as
        // null.
        var expectedResults = [
            {_id: 0, a: 1, "same": [{_id: 0, b: 1}]},
            {_id: 1, a: null, "same": [{_id: 1, b: null}, {_id: 2}]},
            {_id: 2, "same": [{_id: 1, b: null}, {_id: 2}]}
        ];
        testPipeline([{$lookup: {localField: "a", foreignField: "b", from: "from", as: "same"}}],
                     expectedResults,
                     coll);

        // If localField is nonexistent, it is treated as if it is null.
        expectedResults = [
            {_id: 0, a: 1, "same": [{_id: 1, b: null}, {_id: 2}]},
            {_id: 1, a: null, "same": [{_id: 1, b: null}, {_id: 2}]},
            {_id: 2, "same": [{_id: 1, b: null}, {_id: 2}]}
        ];
        testPipeline(
            [{$lookup: {localField: "nonexistent", foreignField: "b", from: "from", as: "same"}}],
            expectedResults,
            coll);

        // If foreignField is nonexistent, it is treated as if it is null.
        expectedResults = [
            {_id: 0, a: 1, "same": []},
            {_id: 1, a: null, "same": [{_id: 0, b: 1}, {_id: 1, b: null}, {_id: 2}]},
            {_id: 2, "same": [{_id: 0, b: 1}, {_id: 1, b: null}, {_id: 2}]}
        ];
        testPipeline(
            [{$lookup: {localField: "a", foreignField: "nonexistent", from: "from", as: "same"}}],
            expectedResults,
            coll);

        // If there are no matches or the from coll doesn't exist, the result is an empty array.
        expectedResults =
            [{_id: 0, a: 1, "same": []}, {_id: 1, a: null, "same": []}, {_id: 2, "same": []}];
        testPipeline(
            [{$lookup: {localField: "_id", foreignField: "nonexistent", from: "from", as: "same"}}],
            expectedResults,
            coll);
        testPipeline(
            [{$lookup: {localField: "a", foreignField: "b", from: "nonexistent", as: "same"}}],
            expectedResults,
            coll);

        // If field name specified by "as" already exists, it is overwritten.
        expectedResults = [
            {_id: 0, "a": [{_id: 0, b: 1}]},
            {_id: 1, "a": [{_id: 1, b: null}, {_id: 2}]},
            {_id: 2, "a": [{_id: 1, b: null}, {_id: 2}]}
        ];
        testPipeline([{$lookup: {localField: "a", foreignField: "b", from: "from", as: "a"}}],
                     expectedResults,
                     coll);

        // Running multiple $lookups in the same pipeline is allowed.
        expectedResults = [
            {_id: 0, a: 1, "c": [{_id: 0, b: 1}], "d": [{_id: 0, b: 1}]},
            {
              _id: 1,
              a: null, "c": [{_id: 1, b: null}, {_id: 2}], "d": [{_id: 1, b: null}, {_id: 2}]
            },
            {_id: 2, "c": [{_id: 1, b: null}, {_id: 2}], "d": [{_id: 1, b: null}, {_id: 2}]}
        ];
        testPipeline(
            [
              {$lookup: {localField: "a", foreignField: "b", from: "from", as: "c"}},
              {$project: {"a": 1, "c": 1}},
              {$lookup: {localField: "a", foreignField: "b", from: "from", as: "d"}}
            ],
            expectedResults,
            coll);

        //
        // Coalescing with $unwind.
        //

        // A normal $unwind with on the "as" field.
        expectedResults = [
            {_id: 0, a: 1, same: {_id: 0, b: 1}},
            {_id: 1, a: null, same: {_id: 1, b: null}},
            {_id: 1, a: null, same: {_id: 2}},
            {_id: 2, same: {_id: 1, b: null}},
            {_id: 2, same: {_id: 2}}
        ];
        testPipeline(
            [
              {$lookup: {localField: "a", foreignField: "b", from: "from", as: "same"}},
              {$unwind: {path: "$same"}}
            ],
            expectedResults,
            coll);

        // An $unwind on the "as" field, with includeArrayIndex.
        expectedResults = [
            {_id: 0, a: 1, same: {_id: 0, b: 1}, index: NumberLong(0)},
            {_id: 1, a: null, same: {_id: 1, b: null}, index: NumberLong(0)},
            {_id: 1, a: null, same: {_id: 2}, index: NumberLong(1)},
            {_id: 2, same: {_id: 1, b: null}, index: NumberLong(0)},
            {_id: 2, same: {_id: 2}, index: NumberLong(1)},
        ];
        testPipeline(
            [
              {$lookup: {localField: "a", foreignField: "b", from: "from", as: "same"}},
              {$unwind: {path: "$same", includeArrayIndex: "index"}}
            ],
            expectedResults,
            coll);

        // Normal $unwind with no matching documents.
        expectedResults = [];
        testPipeline(
            [
              {$lookup: {localField: "_id", foreignField: "nonexistent", from: "from", as: "same"}},
              {$unwind: {path: "$same"}}
            ],
            expectedResults,
            coll);

        // $unwind with preserveNullAndEmptyArray with no matching documents.
        expectedResults = [
            {_id: 0, a: 1},
            {_id: 1, a: null},
            {_id: 2},
        ];
        testPipeline(
            [
              {$lookup: {localField: "_id", foreignField: "nonexistent", from: "from", as: "same"}},
              {$unwind: {path: "$same", preserveNullAndEmptyArrays: true}}
            ],
            expectedResults,
            coll);

        // $unwind with preserveNullAndEmptyArray, some with matching documents, some without.
        expectedResults = [
            {_id: 0, a: 1},
            {_id: 1, a: null, same: {_id: 0, b: 1}},
            {_id: 2},
        ];
        testPipeline(
            [
              {$lookup: {localField: "_id", foreignField: "b", from: "from", as: "same"}},
              {$unwind: {path: "$same", preserveNullAndEmptyArrays: true}}
            ],
            expectedResults,
            coll);

        // $unwind with preserveNullAndEmptyArray and includeArrayIndex, some with matching
        // documents, some without.
        expectedResults = [
            {_id: 0, a: 1, index: null},
            {_id: 1, a: null, same: {_id: 0, b: 1}, index: NumberLong(0)},
            {_id: 2, index: null},
        ];
        testPipeline(
            [
              {$lookup: {localField: "_id", foreignField: "b", from: "from", as: "same"}},
              {
                $unwind:
                    {path: "$same", preserveNullAndEmptyArrays: true, includeArrayIndex: "index"}
              }
            ],
            expectedResults,
            coll);

        //
        // Dependencies.
        //

        // If $lookup didn't add "localField" to its dependencies, this test would fail as the
        // value of the "a" field would be lost and treated as null.
        expectedResults = [
            {_id: 0, "same": [{_id: 0, b: 1}]},
            {_id: 1, "same": [{_id: 1, b: null}, {_id: 2}]},
            {_id: 2, "same": [{_id: 1, b: null}, {_id: 2}]}
        ];
        testPipeline(
            [
              {$lookup: {localField: "a", foreignField: "b", from: "from", as: "same"}},
              {$project: {"same": 1}}
            ],
            expectedResults,
            coll);

        // If $lookup didn't add fields referenced by "let" variables to its dependencies, this test
        // would fail as the value of the "a" field would be lost and treated as null.
        expectedResults = [
            {"_id": 0, "same": [{"_id": 0, "x": 1}, {"_id": 1, "x": 1}, {"_id": 2, "x": 1}]},
            {
              "_id": 1,
              "same": [{"_id": 0, "x": null}, {"_id": 1, "x": null}, {"_id": 2, "x": null}]
            },
            {"_id": 2, "same": [{"_id": 0}, {"_id": 1}, {"_id": 2}]}
        ];
        testPipeline(
            [
              {
                $lookup: {
                    let : {var1: "$a"},
                    pipeline: [{$project: {x: "$$var1"}}],
                    from: "from",
                    as: "same"
                }
              },
              {$project: {"same": 1}}
            ],
            expectedResults,
            coll);

        //
        // Dotted field paths.
        //

        coll.drop();
        assert.writeOK(coll.insert({_id: 0, a: 1}));
        assert.writeOK(coll.insert({_id: 1, a: null}));
        assert.writeOK(coll.insert({_id: 2}));
        assert.writeOK(coll.insert({_id: 3, a: {c: 1}}));

        from.drop();
        assert.writeOK(from.insert({_id: 0, b: 1}));
        assert.writeOK(from.insert({_id: 1, b: null}));
        assert.writeOK(from.insert({_id: 2}));
        assert.writeOK(from.insert({_id: 3, b: {c: 1}}));
        assert.writeOK(from.insert({_id: 4, b: {c: 2}}));

        // Once without a dotted field.
        var pipeline = [{$lookup: {localField: "a", foreignField: "b", from: "from", as: "same"}}];
        expectedResults = [
            {_id: 0, a: 1, "same": [{_id: 0, b: 1}]},
            {_id: 1, a: null, "same": [{_id: 1, b: null}, {_id: 2}]},
            {_id: 2, "same": [{_id: 1, b: null}, {_id: 2}]},
            {_id: 3, a: {c: 1}, "same": [{_id: 3, b: {c: 1}}]}
        ];
        testPipeline(pipeline, expectedResults, coll);

        // Look up a dotted field.
        pipeline = [{$lookup: {localField: "a.c", foreignField: "b.c", from: "from", as: "same"}}];
        // All but the last document in 'coll' have a nullish value for 'a.c'.
        expectedResults = [
            {_id: 0, a: 1, same: [{_id: 0, b: 1}, {_id: 1, b: null}, {_id: 2}]},
            {_id: 1, a: null, same: [{_id: 0, b: 1}, {_id: 1, b: null}, {_id: 2}]},
            {_id: 2, same: [{_id: 0, b: 1}, {_id: 1, b: null}, {_id: 2}]},
            {_id: 3, a: {c: 1}, same: [{_id: 3, b: {c: 1}}]}
        ];
        testPipeline(pipeline, expectedResults, coll);

        // With an $unwind stage.
        coll.drop();
        assert.writeOK(coll.insert({_id: 0, a: {b: 1}}));
        assert.writeOK(coll.insert({_id: 1}));

        from.drop();
        assert.writeOK(from.insert({_id: 0, target: 1}));

        pipeline = [
            {
              $lookup: {
                  localField: "a.b",
                  foreignField: "target",
                  from: "from",
                  as: "same.documents",
              }
            },
            {
              // Expected input to $unwind:
              // {_id: 0, a: {b: 1}, same: {documents: [{_id: 0, target: 1}]}}
              // {_id: 1, same: {documents: []}}
              $unwind: {
                  path: "$same.documents",
                  preserveNullAndEmptyArrays: true,
                  includeArrayIndex: "c.d.e",
              }
            }
        ];
        expectedResults = [
            {_id: 0, a: {b: 1}, same: {documents: {_id: 0, target: 1}}, c: {d: {e: NumberLong(0)}}},
            {_id: 1, same: {}, c: {d: {e: null}}},
        ];
        testPipeline(pipeline, expectedResults, coll);

        //
        // Query-like local fields (SERVER-21287)
        //

        // This must only do an equality match rather than treating the value as a regex.
        coll.drop();
        assert.writeOK(coll.insert({_id: 0, a: /a regex/}));

        from.drop();
        assert.writeOK(from.insert({_id: 0, b: /a regex/}));
        assert.writeOK(from.insert({_id: 1, b: "string that matches /a regex/"}));

        pipeline = [
            {
              $lookup: {
                  localField: "a",
                  foreignField: "b",
                  from: "from",
                  as: "b",
              }
            },
        ];
        expectedResults = [{_id: 0, a: /a regex/, b: [{_id: 0, b: /a regex/}]}];
        testPipeline(pipeline, expectedResults, coll);

        //
        // A local value of an array.
        //

        // Basic array corresponding to multiple documents.
        coll.drop();
        assert.writeOK(coll.insert({_id: 0, a: [0, 1, 2]}));

        from.drop();
        assert.writeOK(from.insert({_id: 0}));
        assert.writeOK(from.insert({_id: 1}));

        pipeline = [
            {
              $lookup: {
                  localField: "a",
                  foreignField: "_id",
                  from: "from",
                  as: "b",
              }
            },
        ];
        expectedResults = [{_id: 0, a: [0, 1, 2], b: [{_id: 0}, {_id: 1}]}];
        testPipeline(pipeline, expectedResults, coll);

        // Basic array corresponding to a single document.
        coll.drop();
        assert.writeOK(coll.insert({_id: 0, a: [1]}));

        from.drop();
        assert.writeOK(from.insert({_id: 0}));
        assert.writeOK(from.insert({_id: 1}));

        pipeline = [
            {
              $lookup: {
                  localField: "a",
                  foreignField: "_id",
                  from: "from",
                  as: "b",
              }
            },
        ];
        expectedResults = [{_id: 0, a: [1], b: [{_id: 1}]}];
        testPipeline(pipeline, expectedResults, coll);

        // Array containing regular expressions.
        coll.drop();
        assert.writeOK(coll.insert({_id: 0, a: [/a regex/, /^x/]}));
        assert.writeOK(coll.insert({_id: 1, a: [/^x/]}));

        from.drop();
        assert.writeOK(from.insert({_id: 0, b: "should not match a regex"}));
        assert.writeOK(from.insert({_id: 1, b: "xxxx"}));
        assert.writeOK(from.insert({_id: 2, b: /a regex/}));
        assert.writeOK(from.insert({_id: 3, b: /^x/}));

        pipeline = [
            {
              $lookup: {
                  localField: "a",
                  foreignField: "b",
                  from: "from",
                  as: "b",
              }
            },
        ];
        expectedResults = [
            {_id: 0, a: [/a regex/, /^x/], b: [{_id: 2, b: /a regex/}, {_id: 3, b: /^x/}]},
            {_id: 1, a: [/^x/], b: [{_id: 3, b: /^x/}]}
        ];
        testPipeline(pipeline, expectedResults, coll);

        // 'localField' references a field within an array of sub-objects.
        coll.drop();
        assert.writeOK(coll.insert({_id: 0, a: [{b: 1}, {b: 2}]}));

        from.drop();
        assert.writeOK(from.insert({_id: 0}));
        assert.writeOK(from.insert({_id: 1}));
        assert.writeOK(from.insert({_id: 2}));
        assert.writeOK(from.insert({_id: 3}));

        pipeline = [
            {
              $lookup: {
                  localField: "a.b",
                  foreignField: "_id",
                  from: "from",
                  as: "c",
              }
            },
        ];

        expectedResults = [{"_id": 0, "a": [{"b": 1}, {"b": 2}], "c": [{"_id": 1}, {"_id": 2}]}];
        testPipeline(pipeline, expectedResults, coll);

        //
        // Pipeline syntax using 'let' variables.
        //
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, x: 1}));
        assert.writeOK(coll.insert({_id: 2, x: 2}));
        assert.writeOK(coll.insert({_id: 3, x: 3}));

        from.drop();
        assert.writeOK(from.insert({_id: 1}));
        assert.writeOK(from.insert({_id: 2}));
        assert.writeOK(from.insert({_id: 3}));

        // Basic non-equi theta join via $project.
        pipeline = [
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
                            pipeline: [
                                {$match: {$expr: {$gt: ["$_id", "$$var2"]}}},
                            ],
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
                      {$project: {newField: 0}}
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

        // The dotted path offset of a non-object variable is equivalent referencing an undefined
        // field.
        pipeline = [
            {
              $lookup: {
                  let : {var1: "$x"},
                  pipeline: [
                      {
                        $match: {
                            $expr: {
                                $eq: [
                                    "FIELD-IS-NULL",
                                    {$ifNull: ["$$var1.y.z", "FIELD-IS-NULL"]}
                                ]
                            }
                        }
                      },
                  ],
                  from: "from",
                  as: "as",
              }
            },
            {$project: {_id: 0}}
        ];

        expectedResults = [
            {"x": 1, "as": [{"_id": 1}, {"_id": 2}, {"_id": 3}]},
            {"x": 2, "as": [{"_id": 1}, {"_id": 2}, {"_id": 3}]},
            {"x": 3, "as": [{"_id": 1}, {"_id": 2}, {"_id": 3}]}
        ];
        testPipeline(pipeline, expectedResults, coll);

        // Comparison where a 'let' variable references an array.
        coll.drop();
        assert.writeOK(coll.insert({x: [1, 2, 3]}));

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
        assert.writeOK(coll.insert({x: {y: {z: 10}}}));

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

        expectedResults = [{
            "x": {"y": {"z": 10}},
            "as": [{"_id": 1, "z": 10}, {"_id": 2, "z": 10}, {"_id": 3, "z": 10}]
        }];
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
                  pipeline: [
                      {$match: {$expr: {$eq: ["$$var1", "$$CURRENT.x.y.z"]}}},
                      {$project: {_id: 0}}
                  ],
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
        assert.writeOK(coll.insert({_id: 1, w: 1}));
        assert.writeOK(coll.insert({_id: 2, w: 2}));
        assert.writeOK(coll.insert({_id: 3, w: 3}));

        from.drop();
        assert.writeOK(from.insert({_id: 1, x: 1}));
        assert.writeOK(from.insert({_id: 2, x: 2}));
        assert.writeOK(from.insert({_id: 3, x: 3}));

        thirdColl.drop();
        assert.writeOK(thirdColl.insert({_id: 1, y: 1}));
        assert.writeOK(thirdColl.insert({_id: 2, y: 2}));
        assert.writeOK(thirdColl.insert({_id: 3, y: 3}));

        fourthColl.drop();
        assert.writeOK(fourthColl.insert({_id: 1, z: 1}));
        assert.writeOK(fourthColl.insert({_id: 2, z: 2}));
        assert.writeOK(fourthColl.insert({_id: 3, z: 3}));

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
            "firstLookup": [{
                "_id": 2,
                x: 2, "secondLookup": [{"_id": 3, y: 3, "thirdLookup": [{_id: 1, z: 1}]}]
            }]
        }];
        testPipeline(pipeline, expectedResults, coll);

        // Deeply nested $lookup pipeline. Confirm that we can execute an aggregation with nested
        // $lookup sub-pipelines up to the maximum depth, but not beyond.
        let nestedPipeline = generateNestedPipeline("lookup", 20);
        assert.commandWorked(coll.getDB().runCommand(
            {aggregate: coll.getName(), pipeline: nestedPipeline, cursor: {}}));

        nestedPipeline = generateNestedPipeline("lookup", 21);
        assertErrorCode(coll, nestedPipeline, ErrorCodes.MaxSubPipelineDepthExceeded);

        // Confirm that maximum $lookup sub-pipeline depth is respected when aggregating views whose
        // combined nesting depth exceeds the limit.
        nestedPipeline = generateNestedPipeline("lookup", 10);
        coll.getDB().view1.drop();
        assert.commandWorked(
            coll.getDB().runCommand({create: "view1", viewOn: "lookup", pipeline: nestedPipeline}));

        nestedPipeline = generateNestedPipeline("view1", 10);
        coll.getDB().view2.drop();
        assert.commandWorked(
            coll.getDB().runCommand({create: "view2", viewOn: "view1", pipeline: nestedPipeline}));

        // Confirm that a composite sub-pipeline depth of 20 is allowed.
        assert.commandWorked(
            coll.getDB().runCommand({aggregate: "view2", pipeline: [], cursor: {}}));

        const pipelineWhichExceedsNestingLimit = generateNestedPipeline("view2", 1);
        coll.getDB().view3.drop();
        assert.commandWorked(coll.getDB().runCommand(
            {create: "view3", viewOn: "view2", pipeline: pipelineWhichExceedsNestingLimit}));

        // Confirm that a composite sub-pipeline depth greater than 20 fails.
        assertErrorCode(coll.getDB().view3, [], ErrorCodes.MaxSubPipelineDepthExceeded);

        //
        // Error cases.
        //

        // 'from', 'as', 'localField' and 'foreignField' must all be specified when run with
        // localField/foreignField syntax.
        assertErrorCode(coll,
                        [{$lookup: {foreignField: "b", from: "from", as: "same"}}],
                        ErrorCodes.FailedToParse);
        assertErrorCode(coll,
                        [{$lookup: {localField: "a", from: "from", as: "same"}}],
                        ErrorCodes.FailedToParse);
        assertErrorCode(coll,
                        [{$lookup: {localField: "a", foreignField: "b", as: "same"}}],
                        ErrorCodes.FailedToParse);
        assertErrorCode(coll,
                        [{$lookup: {localField: "a", foreignField: "b", from: "from"}}],
                        ErrorCodes.FailedToParse);

        // localField/foreignField and pipeline/let syntax must not be mixed.
        assertErrorCode(coll,
                        [{$lookup: {pipeline: [], foreignField: "b", from: "from", as: "as"}}],
                        ErrorCodes.FailedToParse);
        assertErrorCode(coll,
                        [{$lookup: {pipeline: [], localField: "b", from: "from", as: "as"}}],
                        ErrorCodes.FailedToParse);
        assertErrorCode(
            coll,
            [{$lookup: {pipeline: [], localField: "b", foreignField: "b", from: "from", as: "as"}}],
            ErrorCodes.FailedToParse);
        assertErrorCode(coll,
                        [{$lookup: {let : {a: "$b"}, foreignField: "b", from: "from", as: "as"}}],
                        ErrorCodes.FailedToParse);
        assertErrorCode(coll,
                        [{$lookup: {let : {a: "$b"}, localField: "b", from: "from", as: "as"}}],
                        ErrorCodes.FailedToParse);
        assertErrorCode(
            coll,
            [{
               $lookup:
                   {let : {a: "$b"}, localField: "b", foreignField: "b", from: "from", as: "as"}
            }],
            ErrorCodes.FailedToParse);

        // 'from', 'as', 'localField' and 'foreignField' must all be of type string.
        assertErrorCode(coll,
                        [{$lookup: {localField: 1, foreignField: "b", from: "from", as: "as"}}],
                        ErrorCodes.FailedToParse);
        assertErrorCode(coll,
                        [{$lookup: {localField: "a", foreignField: 1, from: "from", as: "as"}}],
                        ErrorCodes.FailedToParse);
        assertErrorCode(coll,
                        [{$lookup: {localField: "a", foreignField: "b", from: 1, as: "as"}}],
                        ErrorCodes.FailedToParse);
        assertErrorCode(coll,
                        [{$lookup: {localField: "a", foreignField: "b", from: "from", as: 1}}],
                        ErrorCodes.FailedToParse);

        // 'pipeline' and 'let' must be of expected type.
        assertErrorCode(
            coll, [{$lookup: {pipeline: 1, from: "from", as: "as"}}], ErrorCodes.TypeMismatch);
        assertErrorCode(
            coll, [{$lookup: {pipeline: {}, from: "from", as: "as"}}], ErrorCodes.TypeMismatch);
        assertErrorCode(coll,
                        [{$lookup: {let : 1, pipeline: [], from: "from", as: "as"}}],
                        ErrorCodes.FailedToParse);
        assertErrorCode(coll,
                        [{$lookup: {let : [], pipeline: [], from: "from", as: "as"}}],
                        ErrorCodes.FailedToParse);

        // The foreign collection must be a valid namespace.
        assertErrorCode(coll,
                        [{$lookup: {localField: "a", foreignField: "b", from: "", as: "as"}}],
                        ErrorCodes.InvalidNamespace);
        // $lookup's field must be an object.
        assertErrorCode(coll, [{$lookup: "string"}], ErrorCodes.FailedToParse);
    }

    // Run tests on single node.
    db.lookUp.drop();
    db.from.drop();
    db.thirdColl.drop();
    db.fourthColl.drop();
    runTest(db.lookUp, db.from, db.thirdColl, db.fourthColl);

    // Run tests in a sharded environment.
    var sharded = new ShardingTest({shards: 2, mongos: 1});
    assert(sharded.adminCommand({enableSharding: "test"}));
    sharded.getDB('test').lookUp.drop();
    sharded.getDB('test').from.drop();
    sharded.getDB('test').thirdColl.drop();
    sharded.getDB('test').fourthColl.drop();
    assert(sharded.adminCommand({shardCollection: "test.lookUp", key: {_id: 'hashed'}}));
    runTest(sharded.getDB('test').lookUp,
            sharded.getDB('test').from,
            sharded.getDB('test').thirdColl,
            sharded.getDB('test').fourthColl);

    // An error is thrown if the from collection is sharded.
    assert(sharded.adminCommand({shardCollection: "test.from", key: {_id: 1}}));
    assertErrorCode(sharded.getDB('test').lookUp,
                    [{$lookup: {localField: "a", foreignField: "b", from: "from", as: "same"}}],
                    28769);

    // An error is thrown if nested $lookup from collection is sharded.
    assert(sharded.adminCommand({shardCollection: "test.fourthColl", key: {_id: 1}}));
    assertErrorCode(sharded.getDB('test').lookUp,
                    [{
                       $lookup: {
                           pipeline: [{$lookup: {pipeline: [], from: "fourthColl", as: "same"}}],
                           from: "thirdColl",
                           as: "same"
                       }
                    }],
                    28769);

    sharded.stop();
}());
