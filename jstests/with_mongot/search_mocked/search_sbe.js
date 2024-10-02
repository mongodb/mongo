/**
 * Tests basic functionality of pushing down $search into SBE.
 * @tags: [featureFlagSbeFull]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {getAggPlanStages, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {assertEngine} from "jstests/libs/query/analyze_plan.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForQuery,
    MongotMock,
    mongotResponseForBatch
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

const dbName = "test";

// Set up mongotmock and point the mongod to it.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

const conn = MongoRunner.runMongod({
    setParameter:
        {mongotHost: mongotConn.host, featureFlagSearchInSbe: true, featureFlagSbeFull: true}
});
const db = conn.getDB("test");
const coll = db[jsTestName()];
const collForeign = db[jsTestName() + "_foreign"];
coll.drop();
collForeign.drop();

assert.commandWorked(coll.insert({"_id": 1, a: "Twinkle twinkle little star", b: [1]}));
assert.commandWorked(coll.insert({"_id": 2, a: "How I wonder what you are", b: [2, 5]}));
assert.commandWorked(coll.insert({"_id": 3, a: "You're a star!", b: [1, 3, 4]}));
assert.commandWorked(coll.insert({"_id": 4, a: "A star is born.", b: [2, 4, 6]}));
assert.commandWorked(coll.insert({"_id": 5, a: "Up above the world so high", b: 5}));
assert.commandWorked(coll.insert({"_id": 6, a: "Sun, moon and stars", b: 6}));

for (let i = 1; i <= 6; ++i) {
    assert.commandWorked(collForeign.insert({"_id": i}));
}

const collUUID = getUUIDFromListCollections(db, coll.getName());
const searchQuery1 = {
    query: "star",
    path: "a"
};
const searchCmd1 = {
    search: coll.getName(),
    collectionUUID: collUUID,
    query: searchQuery1,
    $db: "test"
};
const searchQuery2 = {
    query: "re",
    path: "a"
};
const searchCmd2 = {
    search: coll.getName(),
    collectionUUID: collUUID,
    query: searchQuery2,
    $db: "test"
};

const explainContents = {
    profession: "writer"
};

const cursorId = NumberLong(123);
const history1 = [
    {
        expectedCommand: searchCmd1,
        response: {
            cursor: {
                id: NumberLong(0),
                ns: coll.getFullName(),
                nextBatch: [
                    {_id: 1, $searchScore: 0.321},
                    {_id: 3, $searchScore: 0.654},
                    {_id: 4, $searchScore: 0.789},
                    // '_id' doesn't exist in db, it should be ignored.
                    {_id: 8, $searchScore: 0.891},
                    {_id: 6, $searchScore: 0.891}
                ]
            },
            ok: 1
        }
    },
];
const expected1 = [
    {"_id": 1, a: "Twinkle twinkle little star", b: [1]},
    {"_id": 3, a: "You're a star!", b: [1, 3, 4]},
    {"_id": 4, a: "A star is born.", b: [2, 4, 6]},
    {"_id": 6, a: "Sun, moon and stars", b: 6},
];

const history2 = [
    {
        expectedCommand: searchCmd2,
        response: {
            cursor: {
                id: cursorId,
                ns: coll.getFullName(),
                nextBatch: [
                    {_id: 2, $searchScore: 0.321},
                ]
            },
            ok: 1
        }
    },
    {
        expectedCommand: {getMore: cursorId, collection: coll.getName()},
        response: {
            ok: 1,
            cursor: {
                id: NumberLong(0),
                ns: coll.getFullName(),
                nextBatch: [{_id: 3, $searchScore: 0.345}]
            },
        }
    },
];
const expected2 = [
    {"_id": 2, a: "How I wonder what you are", b: [2, 5]},
    {"_id": 3, a: "You're a star!", b: [1, 3, 4]},
];

const pipeline1 = [{$search: searchQuery1}];
const pipeline2 = [{$search: searchQuery2}];

// Test SBE pushdown.
{
    const history = [{
        expectedCommand: mongotCommandForQuery({
            query: searchQuery1,
            collName: coll.getName(),
            db: "test",
            collectionUUID: collUUID,
            explainVerbosity: {verbosity: "queryPlanner"}
        }),
        response: {explain: explainContents, ok: 1}
    }];
    assert.commandWorked(mongotConn.adminCommand(
        {setMockResponses: 1, cursorId: NumberLong(123), history: history}));

    const explain = coll.explain().aggregate(pipeline1);
    // We should have a $search stage.
    assert.eq(1, getAggPlanStages(explain, "SEARCH").length, explain);

    assert.commandWorked(mongotConn.adminCommand(
        {setMockResponses: 1, cursorId: NumberLong(123), history: history}));
    // $search uses SBE engine.
    assertEngine(pipeline1, "sbe" /* engine */, coll);
}

// Test SBE plan cache.
{
    const getCacheHit = function() {
        return db.serverStatus().metrics.query.planCache.sbe.hits;
    };

    coll.getPlanCache().clear();
    assert.eq(0, coll.getPlanCache().list().length);
    const oldHits = getCacheHit();

    assert.commandWorked(mongotConn.adminCommand(
        {setMockResponses: 1, cursorId: NumberLong(123), history: history1}));
    var res = coll.aggregate(pipeline1);
    assert.eq(expected1, res.toArray());
    // Verify that the cache has 1 entry
    assert.eq(1, coll.getPlanCache().list().length);

    // Re-run the same query.
    assert.commandWorked(mongotConn.adminCommand(
        {setMockResponses: 1, cursorId: NumberLong(123), history: history1}));
    res = coll.aggregate(pipeline1);
    assert.eq(expected1, res.toArray());
    // Verify that the cache has 1 entry, and has been hit for one time.
    assert.eq(1, coll.getPlanCache().list().length);
    assert.eq(getCacheHit(), oldHits + 1);

    // Run a different search query.
    assert.commandWorked(mongotConn.adminCommand(
        {setMockResponses: 1, cursorId: NumberLong(123), history: history2}));
    res = coll.aggregate(pipeline2);
    assert.eq(expected2, res.toArray());
    // Cache not get updated.
    assert.eq(1, coll.getPlanCache().list().length);
    // Hits stats is incremented.
    assert.eq(getCacheHit(), oldHits + 2);
}

// Test how do we handle the case that _id is missing.
{
    const history = [
        {
            expectedCommand: searchCmd1,
            response: {
                cursor: {
                    id: NumberLong(0),
                    ns: coll.getFullName(),
                    nextBatch: [
                        {haha: 1, $searchScore: 0.321},
                    ]
                },
                ok: 1
            }
        },
    ];
    assert.commandWorked(mongotConn.adminCommand(
        {setMockResponses: 1, cursorId: NumberLong(123), history: history}));
    assert.eq([], coll.aggregate(pipeline1).toArray());
}

// Test $search in $lookup sub-pipeline.
{
    const lookupPipeline = [
        {
            $lookup: {
                from: coll.getName(),
                localField: "_id",
                foreignField: "b",
                as: "out",
                pipeline: [
                    {$search: searchQuery1},
                    {
                        $project: {
                            "_id": 0,
                        }
                    }
                ]
            }
        }];
    const lookupExpected = [
        {
            "_id": 1,
            "a": "Twinkle twinkle little star",
            "b": [1],
            "out": [
                {"a": "Twinkle twinkle little star", "b": [1]},
                {"a": "You're a star!", "b": [1, 3, 4]}
            ]
        },
        {
            "_id": 2,
            "a": "How I wonder what you are",
            "b": [2, 5],
            "out": [{"a": "A star is born.", "b": [2, 4, 6]}]
        },
        {
            "_id": 3,
            "a": "You're a star!",
            "b": [1, 3, 4],
            "out": [{"a": "You're a star!", "b": [1, 3, 4]}]
        },
        {
            "_id": 4,
            "a": "A star is born.",
            "b": [2, 4, 6],
            "out":
                [{"a": "You're a star!", "b": [1, 3, 4]}, {"a": "A star is born.", "b": [2, 4, 6]}]
        },
        {"_id": 5, "a": "Up above the world so high", "b": 5, "out": []},
        {
            "_id": 6,
            "a": "Sun, moon and stars",
            "b": 6,
            "out": [{"a": "A star is born.", "b": [2, 4, 6]}, {"a": "Sun, moon and stars", "b": 6}]
        }
    ];

    for (let i = 0; i < 6; i++) {
        assert.commandWorked(mongotConn.adminCommand(
            {setMockResponses: 1, cursorId: NumberLong(123 + i), history: history1}));
    }

    const disableClassicSearch = configureFailPoint(db, 'failClassicSearch');
    // Make sure the classic search fails.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));
    assert.throwsWithCode(() => coll.aggregate(lookupPipeline), 7942401);
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}));

    for (let i = 0; i < 6; i++) {
        assert.commandWorked(mongotConn.adminCommand(
            {setMockResponses: 1, cursorId: NumberLong(123 + i), history: history1}));
    }
    // This should run in SBE.
    assert.eq(lookupExpected, coll.aggregate(lookupPipeline).toArray());

    disableClassicSearch.off();
}

// Test $search on a non-existent collection in SBE pushdown.
{
    const unionWithPipeline = [
        {$match: {b: 10}},
        {
            $unionWith: {
                coll: "unknown_collection",
                pipeline: [
                    {$search: searchQuery1},
                    {
                        $project: {
                            "_id": 0,
                        }
                    }
                ]
            }
        }
    ];

    const disableClassicSearch = configureFailPoint(db, 'failClassicSearch');
    // Make sure the classic search fails.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));
    assert.throwsWithCode(() => coll.aggregate(unionWithPipeline), 7942401);
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}));

    // This should run in SBE, and return no data, so no need to setup mock history.
    assert.eq([], coll.aggregate(unionWithPipeline).toArray());

    disableClassicSearch.off();
}

function runSearchTest(mongotQuery, pipeline, mongotResponseBatch, expectedDocs) {
    const cursorId = NumberLong(123);
    const responseOk = 1;
    const history = [{
        expectedCommand: mongotCommandForQuery(
            {query: mongotQuery, collName: coll.getName(), db: dbName, collectionUUID: collUUID}),
        response: mongotResponseForBatch(
            mongotResponseBatch, NumberLong(0), coll.getFullName(), responseOk),
    }];
    mongotmock.setMockResponses(history, cursorId);
    assertArrayEq({actual: coll.aggregate(pipeline).toArray(), expected: expectedDocs});

    // Check the stage is pushed down into SBE.
    const explainHistory = [{
        expectedCommand: mongotCommandForQuery({
            query: mongotQuery,
            collName: coll.getName(),
            db: dbName,
            collectionUUID: collUUID,
            explainVerbosity: {verbosity: "queryPlanner"}
        }),
        response: {explain: explainContents, ok: 1}
    }];
    mongotmock.setMockResponses(explainHistory, cursorId);
    return coll.explain().aggregate(pipeline);
}

function testMetaProj(mongotQuery, pipeline) {
    const highlights = ["a", "b", "c"];
    const searchScoreDetails = {value: 1.234, description: "the score is great", details: []};
    const mongotResponseBatch = [{
        _id: 1,
        $searchScore: 1.234,
        $searchHighlights: highlights,
        $searchScoreDetails: searchScoreDetails
    }];
    const expectedDocs = [{
        _id: 1,
        a: "Twinkle twinkle little star",
        score: 1.234,
        highlights: highlights,
        scoreInfo: searchScoreDetails
    }];
    return runSearchTest(mongotQuery, pipeline, mongotResponseBatch, expectedDocs);
}

// Test $meta projection in SBE.
{
    const mongotQuery = {scoreDetails: true};
    const plan = testMetaProj(mongotQuery, [
        {$search: mongotQuery},
        {
            $project: {
                _id: 1,
                a: 1,
                score: {$meta: "searchScore"},
                highlights: {$meta: "searchHighlights"},
                scoreInfo: {$meta: "searchScoreDetails"}
            }
        }
    ]);
    // Make sure $project is pushed down into SBE.
    assert.eq(2, plan['explainVersion']);
    assert.eq(getWinningPlanFromExplain(plan).stage, "PROJECTION_DEFAULT");
}

// Test $meta projection is not pushed down into SBE.
{
    const mongotQuery = {scoreDetails: true};
    const plan = testMetaProj(mongotQuery, [
        {$search: mongotQuery},
        {$_internalInhibitOptimization: {}},
        {
            $project: {
                _id: 1,
                a: 1,
                score: {$meta: "searchScore"},
                highlights: {$meta: "searchHighlights"},
                scoreInfo: {$meta: "searchScoreDetails"}
            }
        }
    ]);
    // Make sure $project is not pushed down into SBE.
    assert.eq(1, getAggPlanStages(plan, "$project").length, plan);
}

// Test $search followed by $group that are both pushed down into SBE.
{
    const mongotQuery = {scoreDetails: true};
    const pipeline = [
        {$search: mongotQuery},
        {
            $project: {
                _id: 0,
                a: 1,
                score: {$meta: "searchScore"},
            }
        },
        {$group: {_id: "$score", count: {$count: {}}}}
    ];
    const mongotResponseBatch = [
        {
            _id: 1,
            $searchScore: 1.234,
        },
        {
            _id: 2,
            $searchScore: 1.234,
        },
        {
            _id: 3,
            $searchScore: 3.2,
        },
        {
            _id: 4,
            $searchScore: 0.5,
        },
        {
            _id: 5,
            $searchScore: 1.234,
        },
        {
            _id: 6,
            $searchScore: 0.5,
        }
    ];
    const expectedDocs = [
        {
            _id: 1.234,
            count: 3,
        },
        {
            _id: 0.5,
            count: 2,
        },
        {
            _id: 3.2,
            count: 1,
        }
    ];

    const plan = runSearchTest(mongotQuery, pipeline, mongotResponseBatch, expectedDocs);
    assert.eq(2, plan['explainVersion']);
    // Make sure $group is pushed down into SBE.
    assert.eq(getWinningPlanFromExplain(plan).stage, "GROUP", plan);
}

// Test $meta after $group in SBE, metadata are exhausted by group.
{
    const mongotQuery = {scoreDetails: true};
    const pipeline = [
        {$search: mongotQuery},
        {$group: {_id: null, count: {$count: {}}}},
        {
            $project: {
                _id: 0,
                score: {$meta: "searchScore"},
            }
        },
    ];
    runSearchTest(mongotQuery,
                  pipeline,
                  [{
                      _id: 1,
                      $searchScore: 1.234,
                  }],
                  [{}]);
}

// Test $meta after $group but $meta is not pushed down, metadata are exhausted by group.
{
    const mongotQuery = {scoreDetails: true};
    const pipeline = [
        {$search: mongotQuery},
        {$group: {_id: null, count: {$count: {}}}},
        {$_internalInhibitOptimization: {}},
        {
            $project: {
                _id: 0,
                score: {$meta: "searchScore"},
            }
        },
    ];
    // Expects empty document because 'score' field is missing.
    runSearchTest(mongotQuery,
                  pipeline,
                  [{
                      _id: 1,
                      $searchScore: 1.234,
                  }],
                  [{}]);
}

// Test $search + $lookup pushdown.
{
    const mongotQuery = {scoreDetails: true};
    const pipeline = [
        {$search: mongotQuery},
        {
            $project: {
                _id: 0,
                a: 1,
                b: 1,
                score: {$meta: "searchScore"},
            }
        },
        {
            $lookup:
                {from: collForeign.getName(), localField: "score", foreignField: "_id", as: "out"}
        }
    ];
    const mongotResponseBatch = [
        {
            _id: 1,
            $searchScore: 2,
        },
        {
            _id: 2,
            $searchScore: 1,
        },
        {
            _id: 3,
            $searchScore: 6,
        },
    ];
    const expectedDocs = [
        {
            "a": "Twinkle twinkle little star",
            "b": [1],
            "score": 2,
            "out": [
                {"_id": 2},
            ]
        },
        {
            "a": "How I wonder what you are",
            "b": [2, 5],
            "score": 1,
            "out": [
                {"_id": 1},
            ]
        },
        {"a": "You're a star!", "b": [1, 3, 4], "score": 6, "out": [{"_id": 6}]},
    ];

    const plan = runSearchTest(mongotQuery, pipeline, mongotResponseBatch, expectedDocs);
    assert.eq(2, plan['explainVersion']);
    // Make sure $lookup is pushed down into SBE.
    assert.eq(getWinningPlanFromExplain(plan).stage, "EQ_LOOKUP", plan);
}

// Test $search + $unwind pushdown.
{
    const mongotQuery = {scoreDetails: true};
    const pipeline = [
        {$search: mongotQuery},
        {
            $project: {
                highlights: {$meta: "searchHighlights"},
            }
        },
        {$unwind: {path: "$highlights"}},
        // $unwind won't be pushed down if it is the last stage due to performance reason, adding an
        // extra stage here to test $search + $unwind in SBE stage builder.
        {$project: {highlights: 1}}
    ];
    const highlights = ["a", "b", "c"];
    const mongotResponseBatch = [
        {
            _id: 1,
            $searchScore: 2,
            $searchHighlights: highlights,
        },
        {
            _id: 2,
            $searchScore: 1,
            $searchHighlights: highlights,
        },
    ];
    const expectedDocs = [
        {"_id": 1, "highlights": "a"},
        {"_id": 1, "highlights": "b"},
        {"_id": 1, "highlights": "c"},
        {"_id": 2, "highlights": "a"},
        {"_id": 2, "highlights": "b"},
        {"_id": 2, "highlights": "c"},
    ];

    const plan = runSearchTest(mongotQuery, pipeline, mongotResponseBatch, expectedDocs);
    assert.eq(2, plan['explainVersion']);
    // Make sure $unwind is pushed down into SBE.
    const winningPlan = getWinningPlanFromExplain(plan);
    assert.eq(winningPlan.stage, "PROJECTION_DEFAULT", plan);
    assert.eq(winningPlan.inputStage.stage, "UNWIND", plan);
}

// Test $search + $replaceRoot pushdown.
{
    const mongotQuery = {scoreDetails: true};
    const pipeline =
        [{$search: mongotQuery}, {$replaceRoot: {newRoot: {$meta: "searchScoreDetails"}}}];
    const searchScoreDetails = {value: 1.234, description: "the score is great", details: []};
    const mongotResponseBatch = [
        {
            _id: 1,
            $searchScore: 1.234,
            $searchScoreDetails: searchScoreDetails,
        },
    ];

    const plan = runSearchTest(mongotQuery, pipeline, mongotResponseBatch, [searchScoreDetails]);
    assert.eq(2, plan['explainVersion']);
    // Make sure $replaceRoot is pushed down into SBE.
    assert.eq(getWinningPlanFromExplain(plan).stage, "REPLACE_ROOT", plan);
}

// Test $search + $match + $sort + $limit + $skip pushdown.
{
    const mongotQuery = {scoreDetails: true};
    const pipeline = [
        {$search: mongotQuery},
        {
            $project: {
                _id: 0,
                a: 1,
                score: {$meta: "searchScore"},
            }
        },
        {$match: {score: {$gt: 1}}},
        {$sort: {score: -1}},
        {$limit: 2},
        {$skip: 1},
    ];
    const mongotResponseBatch = [
        {
            _id: 1,
            $searchScore: 1.234,
        },
        {
            _id: 2,
            $searchScore: 1.334,
        },
        {
            _id: 3,
            $searchScore: 3.2,
        },
        {
            _id: 4,
            $searchScore: 0.5,
        },
        {
            _id: 5,
            $searchScore: 1.534,
        },
        {
            _id: 6,
            $searchScore: 0.5,
        }
    ];
    const expectedDocs = [{"a": "Up above the world so high", "score": 1.534}];

    const plan = runSearchTest(mongotQuery, pipeline, mongotResponseBatch, expectedDocs);
    assert.eq(2, plan['explainVersion']);
    // Make sure $skip is pushed down into SBE, this means all stages are pushed down.
    assert.eq(getWinningPlanFromExplain(plan).stage, "SKIP", plan);
}

// Test $search + $setWindowFields pushdown.
{
    const mongotQuery = {scoreDetails: true};
    const pipeline = [
        {$search: mongotQuery},
        {
            $project: {
                a: 1,
                score: {$meta: "searchScore"},
            }
        },
        {
            $setWindowFields: {
                partitionBy: "$score",
                sortBy: {_id: 1},
                output: {
                    idRank: {
                        $rank: {},
                    }
                }
            }
        }
    ];
    const mongotResponseBatch = [
        {
            _id: 1,
            $searchScore: 1.234,
        },
        {
            _id: 2,
            $searchScore: 1.234,
        },
        {
            _id: 3,
            $searchScore: 3.2,
        },
        {
            _id: 4,
            $searchScore: 0.5,
        },
        {
            _id: 5,
            $searchScore: 1.234,
        },
        {
            _id: 6,
            $searchScore: 0.5,
        }
    ];
    const expectedDocs = [
        {"_id": 4, "a": "A star is born.", "score": 0.5, "idRank": NumberLong(1)},
        {"_id": 6, "a": "Sun, moon and stars", "score": 0.5, "idRank": NumberLong(2)},
        {"_id": 1, "a": "Twinkle twinkle little star", "score": 1.234, "idRank": NumberLong(1)},
        {"_id": 2, "a": "How I wonder what you are", "score": 1.234, "idRank": NumberLong(2)},
        {"_id": 5, "a": "Up above the world so high", "score": 1.234, "idRank": NumberLong(3)},
        {"_id": 3, "a": "You're a star!", "score": 3.2, "idRank": NumberLong(1)}
    ];

    const plan = runSearchTest(mongotQuery, pipeline, mongotResponseBatch, expectedDocs);
    assert.eq(2, plan['explainVersion']);
    // Make sure $setWindowFields is pushed down into SBE.
    assert.eq(getWinningPlanFromExplain(plan).stage, "WINDOW", plan);
}

mongotmock.stop();
MongoRunner.stopMongod(conn);
