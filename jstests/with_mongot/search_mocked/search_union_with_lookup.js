/**
 * Test of `$search` aggregation stage within $unionWith and $lookup stages.
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getAggPlanStage} from "jstests/libs/query/analyze_plan.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {prepareUnionWithExplain} from "jstests/with_mongot/common_utils.js";
import {mongotCommandForQuery, MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    getMongotStagesAndValidateExplainExecutionStats
} from "jstests/with_mongot/mongotmock/lib/utils.js";

// Set up mongotmock and point the mongod to it.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

const conn = MongoRunner.runMongod({setParameter: {mongotHost: mongotConn.host}});
const dbName = jsTestName();
const db = conn.getDB(dbName);
const coll = db.search;
coll.drop();

const collBase = db.base;
collBase.drop();

const collBase2 = db.base2;
collBase2.drop();

const view1 = db.view1;
view1.drop();

assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));
assert.commandWorked(coll.insert({"_id": 2, "title": "cookies and cakes"}));
assert.commandWorked(coll.insert({"_id": 3, "title": "vegetables"}));
assert.commandWorked(coll.insert({"_id": 4, "title": "oranges"}));
assert.commandWorked(coll.insert({"_id": 5, "title": "cakes and oranges"}));
assert.commandWorked(coll.insert({"_id": 6, "title": "cakes and apples"}));
assert.commandWorked(coll.insert({"_id": 7, "title": "apples"}));
assert.commandWorked(coll.insert({"_id": 8, "title": "cakes and kale"}));

assert.commandWorked(collBase.insert({"_id": 100, "localField": "cakes", "weird": false}));
assert.commandWorked(collBase.insert({"_id": 101, "localField": "cakes and kale", "weird": true}));

assert.commandWorked(collBase2.insert({"_id": 200, "ref_id": 1}));
assert.commandWorked(collBase2.insert({"_id": 201, "ref_id": 8}));

const collUUID = getUUIDFromListCollections(db, coll.getName());

var cursorCounter = 123;

/**
 * Term: Search term to query for.
 * Times: Number of commands mongotmock should expect.
 * Batch: Docs for mongotmock to return.
 * searchMetaValue: Value to be used in all the documents as 'vars'. If times >1 and this is an
 * array, length must be equal to times.
 * Returns the query to run.
 */
function setupSearchQuery(
    term, times, batch, searchMetaValue, explainVerbosity = null, explainObject = null) {
    const searchQuery = {query: term, path: "title"};
    const searchCmd = mongotCommandForQuery({
        query: searchQuery,
        collName: coll.getName(),
        db: dbName,
        collectionUUID: collUUID,
        explainVerbosity
    });

    if (Array.isArray(searchMetaValue)) {
        assert.eq(times, searchMetaValue.length);
    }
    // Give mongotmock some stuff to return.
    for (let i = 0; i < times; i++) {
        const cursorId = NumberLong(cursorCounter++);
        let thisMeta = searchMetaValue;
        if (Array.isArray(thisMeta)) {
            thisMeta = searchMetaValue[i];
        }

        let response = {
            cursor: {id: NumberLong(0), ns: coll.getFullName(), nextBatch: batch},
            vars: {SEARCH_META: {value: thisMeta}},
            ok: 1
        };
        if (explainVerbosity != null) {
            response.explain = explainObject;
        }

        let history = {
            expectedCommand: searchCmd,
            response,
        };

        assert.commandWorked(
            mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: [history]}));
    }
    return searchQuery;
}

const makeLookupPipeline = (fromColl, stage, localField) => {
    if (localField) {
        return [
        {$project: {"_id": 0}},
        {
            $lookup: {
                from: fromColl,
                localField: "localField",
                foreignField: "title",
                as: "cake_data",
                pipeline: [
                    stage,
                    {
                        $project: {
                            "_id": 0,
                            "ref_id": "$_id"
                        }
                    }
                ]
            }
        }
    ];
    } else {
        return [
            {$project: {"_id": 0}},
            {
                $lookup: {
                    from: fromColl,
                    let: { local_title: "$localField" },
                    pipeline: [
                        stage,
                        {
                            $match: {
                                $expr: {
                                    $eq: ["$title", "$$local_title"]
                                }
                            }
                        },
                        {
                            $project: {
                                "_id": 0,
                                "ref_id": "$_id"
                            }
                        }
                    ],
                    as: "cake_data"
                }
            }
        ];
    }
};

const makeLookupSearchPipeline = (fromColl, searchQuery, localField) =>
    makeLookupPipeline(fromColl, {$search: searchQuery}, localField);

// Perform a $search query with $lookup.
function searchWithLookup({localField, times}) {
    const lookupSearchQuery = setupSearchQuery("cakes",
                                               times,
                                               [
                                                   {_id: 1, $searchScore: 0.9},
                                                   {_id: 2, $searchScore: 0.8},
                                                   {_id: 5, $searchScore: 0.7},
                                                   {_id: 6, $searchScore: 0.6},
                                                   {_id: 8, $searchScore: 0.5}
                                               ],
                                               1);

    const lookupCursor =
        collBase.aggregate(makeLookupSearchPipeline(coll.getName(), lookupSearchQuery, localField));

    const lookupExpected = [
        {"localField": "cakes", "weird": false, "cake_data": [{"ref_id": 1}]},
        {"localField": "cakes and kale", "weird": true, "cake_data": [{"ref_id": 8}]}
    ];
    assert.sameMembers(lookupExpected, lookupCursor.toArray());
}

// Testing $lookup without cache optimization.
assert.commandWorked(
    db.adminCommand({configureFailPoint: "disablePipelineOptimization", mode: "alwaysOn"}));
searchWithLookup({localField: false, times: 2});
searchWithLookup({localField: true, times: 2});
assert.commandWorked(
    db.adminCommand({configureFailPoint: "disablePipelineOptimization", mode: "off"}));

// Testing $lookup with optimizations on. $lookup executes $search once.
searchWithLookup({localField: false, times: 1});
// Cache optimization doesn't apply when $lookup contains local/foreignField.
searchWithLookup({localField: true, times: 2});

// $search with $lookup with a correlated part of the sub-pipeline ($match).
const lookupWithMatch = setupSearchQuery("cakes",
                                         1,
                                         [
                                             {_id: 1, $searchScore: 0.99},
                                             {_id: 2, $searchScore: 0.20},
                                             {_id: 5, $searchScore: 0.33},
                                             {_id: 6, $searchScore: 0.38},
                                             {_id: 8, $searchScore: 0.45}
                                         ],
                                         ["lookup"]);

let result = assert.commandWorked(db.runCommand({
    aggregate: collBase.getName(),
    pipeline: [
        {$project: {"_id": 0}},
        {
            $lookup: {
                from: coll.getName(),
                let: { local_title: "$localField" },
                pipeline: [{$search: lookupWithMatch}, 
                    {
                        $match: {
                            $expr: {
                                $eq: ["$title", "$$local_title"]
                            }
                        }
                    },
                {
                    $project: {
                        "_id": 0,
                        "ref_id": "$_id",
                        "searchMeta": "$$SEARCH_META",
                    }
                }],
                as: "cake_data",
            }
        }
    ],
    cursor: {}
}));

const lookupWithMatchExpected = [
    {
        "localField": "cakes",
        "weird": false,
        "cake_data": [{"ref_id": 1, "searchMeta": {value: "lookup"}}]
    },
    {
        "localField": "cakes and kale",
        "weird": true,
        "cake_data": [{"ref_id": 8, "searchMeta": {value: "lookup"}}]
    }

];

assert.sameMembers(result.cursor.firstBatch, lookupWithMatchExpected);

const unionSearchQuery = setupSearchQuery("cakes",
                                          1,
                                          [
                                              {_id: 1, $searchScore: 0.9},
                                              {_id: 2, $searchScore: 0.8},
                                              {_id: 5, $searchScore: 0.7},
                                              {_id: 6, $searchScore: 0.6},
                                              {_id: 8, $searchScore: 0.5}
                                          ],
                                          1);

const unionCursor = collBase.aggregate([
    {$project: {"localField": 1, "_id": 0}},
    {$unionWith: {coll: coll.getName(), pipeline: [{$search: unionSearchQuery}]}}
]);

const unionExpected = [
    {"localField": "cakes"},
    {"localField": "cakes and kale"},
    {"_id": 1, "title": "cakes"},
    {"_id": 2, "title": "cookies and cakes"},
    {"_id": 5, "title": "cakes and oranges"},
    {"_id": 6, "title": "cakes and apples"},
    {"_id": 8, "title": "cakes and kale"}
];
assert.sameMembers(unionExpected, unionCursor.toArray());

const multiUnionSearch = setupSearchQuery("cakes",
                                          2,
                                          [
                                              {_id: 1, $searchScore: 0.9},
                                              {_id: 2, $searchScore: 0.8},
                                          ],
                                          ["outer", "inner"]);

// Multiple $search commands with $$SEARCH_META are allowed in a pipeline.
result = assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$search: multiUnionSearch},
        {$project: {_id: 1, meta: "$$SEARCH_META"}},
        {
            $unionWith: {
                coll: coll.getName(),
                pipeline: [{$search: multiUnionSearch}, {$addFields: {meta: "$$SEARCH_META"}}]
            }
        }
    ],
    cursor: {}
}));

const multiUnionExpected = [
    {_id: 1, meta: {value: "outer"}},
    {_id: 2, meta: {value: "outer"}},
    {_id: 1, title: "cakes", meta: {value: "inner"}},
    {_id: 2, title: "cookies and cakes", meta: {value: "inner"}},
];

assert.sameMembers(result.cursor.firstBatch, multiUnionExpected);

const multiLookupSearch = setupSearchQuery("cakes",
                                           2,
                                           [
                                               {_id: 1, $searchScore: 0.9},
                                               {_id: 2, $searchScore: 0.8},
                                           ],
                                           ["outer", "inner"]);
result = assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$search: multiLookupSearch},
        {$project: {_id: 1, meta: "$$SEARCH_META"}},
        {
            $lookup: {
                from: coll.getName(),
                pipeline: [{$search: multiLookupSearch}, {$addFields: {meta: "$$SEARCH_META"}}],
                as: "arr",
            }
        }
    ],
    cursor: {}
}));
const multiLookupExpected = [
    {
        _id: 1,
        meta: {value: "outer"},
        arr: [
            {_id: 1, title: "cakes", meta: {value: "inner"}},
            {_id: 2, title: "cookies and cakes", meta: {value: "inner"}},
        ]
    },
    {
        _id: 2,
        meta: {value: "outer"},
        arr: [
            {_id: 1, title: "cakes", meta: {value: "inner"}},
            {_id: 2, title: "cookies and cakes", meta: {value: "inner"}},
        ]
    },

];

assert.sameMembers(result.cursor.firstBatch, multiLookupExpected);

// $search stage in a sub-pipeline.
const unionSubSearch = setupSearchQuery("cakes",
                                        1,
                                        [
                                            {_id: 1, $searchScore: 0.9},
                                            {_id: 2, $searchScore: 0.8},
                                        ],
                                        "metaVal");
result = assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$match: {val: 1}},  // Matches nothing.
        {
            $unionWith: {
                coll: coll.getName(),
                pipeline: [{$search: unionSubSearch}, {$addFields: {meta: "$$SEARCH_META"}}]
            }
        },
    ],
    cursor: {}
}));

const unionSubSearchExpected = [
    {_id: 1, meta: {value: "metaVal"}, title: "cakes"},
    {_id: 2, meta: {value: "metaVal"}, title: "cookies and cakes"},
];

assert.sameMembers(result.cursor.firstBatch, unionSubSearchExpected);

// Cannot access $$SEARCH_META after a stage with a sub-pipeline.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$match: {val: 1}},
        {$unionWith: {coll: coll.getName(), pipeline: [{$search: {/*Not accessed*/}}]}},
        {$addFields: {meta: "$$SEARCH_META"}},
    ],
    cursor: {}
}),
                             6347901);

// $$SEARCH_META before $unionWith ($search in pipeline).
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$addFields: {meta: "$$SEARCH_META"}},
        {$unionWith: {coll: coll.getName(), pipeline: [{$search: {/*Not accessed*/}}]}},
    ],
    cursor: {}
}),
                             6347902);

// Same test with $lookup.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$match: {val: 1}},
        {
            $lookup: {
                from: coll.getName(),
                pipeline: [{$search: {/*Not accessed*/}}],
                as: "arr",
            }
        },
        {$addFields: {meta: "$$SEARCH_META"}},
    ],
    cursor: {}
}),
                             6347901);

// Test with $lookup ($searchMeta in pipeline).
const lookupSubSearchMeta = setupSearchQuery("cakes",
                                             1,
                                             [
                                                 {_id: 1, $searchScore: 0.9},
                                                 {_id: 2, $searchScore: 0.8},
                                             ],
                                             "metaVal");
result = assert.commandWorked(db.runCommand({
            aggregate: coll.getName(),
            pipeline: [
                {$match: {_id: 1}}, // Match one document.
                {
                    $lookup: {
                        from: coll.getName(),
                        pipeline: [{$searchMeta: lookupSubSearchMeta}],
                        as: "arr",
                    }
                },
            ],
            cursor: {}
        }));

const lookupSubSearchMeta2 = [
    {_id: 1, title: "cakes", arr: [{value: "metaVal"}]},
];

assert.sameMembers(result.cursor.firstBatch, lookupSubSearchMeta2);

// Reset the history for the next test.
let cookiesQuery = setupSearchQuery("cookies",
                                    1,
                                    [
                                        {_id: 2, $searchScore: 0.6},
                                    ],
                                    2);
// This is used in 'unionSearchQuery' in the next test.
setupSearchQuery("cakes",
                 1,
                 [
                     {_id: 1, $searchScore: 0.9},
                     {_id: 2, $searchScore: 0.8},
                     {_id: 5, $searchScore: 0.7},
                     {_id: 6, $searchScore: 0.6},
                     {_id: 8, $searchScore: 0.5}
                 ],
                 1);
// Assert multiple search-type stages are allowed.
const union2Result =
    coll.aggregate([
            {$search: cookiesQuery},
            {$unionWith: {coll: coll.getName(), pipeline: [{$search: unionSearchQuery}]}}
        ])
        .toArray();
const union2SearchExpected = [
    {
        "_id": 2,
        "title": "cookies and cakes",
    },
    {
        "_id": 1,
        "title": "cakes",
    },
    {
        "_id": 2,
        "title": "cookies and cakes",
    },
    {
        "_id": 5,
        "title": "cakes and oranges",
    },
    {
        "_id": 6,
        "title": "cakes and apples",
    },
    {
        "_id": 8,
        "title": "cakes and kale",
    }
];
assert.sameMembers(union2SearchExpected, union2Result);

// $searchMeta in a sub-pipeline is allowed.
const unionSubSearchMeta = setupSearchQuery("cakes",
                                            1,
                                            [
                                                {_id: 1, $searchScore: 0.9},
                                                {_id: 2, $searchScore: 0.8},
                                            ],
                                            "metaVal");
result = assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    cursor: {},
    pipeline: [
        {$match: {_id: 1}},
        {$unionWith: {coll: coll.getName(), pipeline: [{$searchMeta: unionSubSearchMeta}]}}
    ]
}));

const unionSubSearchMetaResults = [
    {_id: 1, title: "cakes"},
    {value: "metaVal"},
];

assert.sameMembers(result.cursor.firstBatch, unionSubSearchMetaResults);

// $searchMeta works in a sub-pipeline with a top-level search.
const searchMetaTestGenericQuery = setupSearchQuery("cakes",
                                                    2,
                                                    [
                                                        {_id: 1, $searchScore: 0.9},
                                                        {_id: 2, $searchScore: 0.8},
                                                    ],
                                                    ["outer", "inner"]);
result = assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$search: searchMetaTestGenericQuery},
        {$project: {_id: 1, meta: "$$SEARCH_META"}},
        {$unionWith: {coll: coll.getName(), pipeline: [{$searchMeta: searchMetaTestGenericQuery}]}}
    ],
    cursor: {}
}));

const searchMetaTestExpected1 = [
    {_id: 1, meta: {value: "outer"}},
    {_id: 2, meta: {value: "outer"}},
    {value: "inner"},
];
assert.sameMembers(result.cursor.firstBatch, searchMetaTestExpected1);

// Same query as last test.
setupSearchQuery("cakes",
                 2,
                 [
                     {_id: 1, $searchScore: 0.9},
                     {_id: 2, $searchScore: 0.8},
                 ],
                 ["outer", "inner"]);
// Multiple $searchMeta in the pipeline works.
result = assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$searchMeta: searchMetaTestGenericQuery},
        {$unionWith: {coll: coll.getName(), pipeline: [{$searchMeta: searchMetaTestGenericQuery}]}}
    ],
    cursor: {}
}));
const searchMetaTestExpected2 = [
    {value: "outer"},
    {value: "inner"},
];
assert.sameMembers(result.cursor.firstBatch, searchMetaTestExpected2);

// Multiple sub-pipelines with $$SEARCH_META are ok.
setupSearchQuery("cakes",
                 3,
                 [
                     {_id: 1, $searchScore: 0.9},
                     {_id: 2, $searchScore: 0.8},
                 ],
                 ["outer", "first", "second"]);
// Multiple $searchMeta in the pipeline works.
result = assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$search: searchMetaTestGenericQuery},
        {$match: {_id: 1}},
        {$project: {_id: 1, title: 1, meta: "$$SEARCH_META"}},
        {$unionWith: {coll: coll.getName(), pipeline: [{$searchMeta: searchMetaTestGenericQuery}]}},
        {$sort: {_id: 1}},  // Arbitrary unrelated stage.
        {$unionWith: {coll: coll.getName(), pipeline: [{$searchMeta: searchMetaTestGenericQuery}]}},
    ],
    cursor: {}
}));
const searchMetaTestExpected3 = [
    {_id: 1, title: "cakes", meta: {value: "outer"}},
    {value: "first"},
    {value: "second"},
];
assert.sameMembers(result.cursor.firstBatch, searchMetaTestExpected3);

// Top level $$SEARCH_META is out of scope after $unionWith.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$search: searchMetaTestGenericQuery},
        {$match: {_id: 1}},
        {$unionWith: {coll: coll.getName(), pipeline: [{$searchMeta: searchMetaTestGenericQuery}]}},
        {$project: {_id: 1, title: 1, meta: "$$SEARCH_META"}},
        {$sort: {_id: 1}},  // Arbitrary unrelated stage.
        {$unionWith: {coll: coll.getName(), pipeline: [{$searchMeta: searchMetaTestGenericQuery}]}},
    ],
    cursor: {}
}),
                             6347901);

// $search within a view use-case with $unionWith.
const viewSearchQuery = setupSearchQuery("cakes",
                                         1,
                                         [
                                             {_id: 1, $searchScore: 0.9},
                                             {_id: 2, $searchScore: 0.8},
                                             {_id: 5, $searchScore: 0.7},
                                             {_id: 6, $searchScore: 0.6},
                                             {_id: 8, $searchScore: 0.5}
                                         ],
                                         1);

assert.commandWorked(db.createView(view1.getName(), collBase.getName(), [
    {$project: {"localField": 1, "_id": 0}},
    {$unionWith: {coll: coll.getName(), pipeline: [{$search: viewSearchQuery}]}}
]));
const viewCursor = view1.aggregate([]);

const viewExpected = [
    {"localField": "cakes"},
    {"localField": "cakes and kale"},
    {"_id": 1, "title": "cakes"},
    {"_id": 2, "title": "cookies and cakes"},
    {"_id": 5, "title": "cakes and oranges"},
    {"_id": 6, "title": "cakes and apples"},
    {"_id": 8, "title": "cakes and kale"}
];
assert.sameMembers(viewExpected, viewCursor.toArray());

// $lookup of a view with $search coll.lookup(searchView, lookup-cond).
const viewSearchQuery2 = setupSearchQuery("cakes",
                                          1,
                                          [
                                              {_id: 1, $searchScore: 0.9},
                                              {_id: 2, $searchScore: 0.8},
                                              {_id: 5, $searchScore: 0.7},
                                              {_id: 6, $searchScore: 0.6},
                                              {_id: 8, $searchScore: 0.5}
                                          ],
                                          1);
view1.drop();
assert.commandWorked(db.createView(view1.getName(), coll.getName(), [{$search: viewSearchQuery2}]));

const lookupViewSearchCursor =
    collBase.aggregate(makeLookupPipeline(view1.getName(), {$project: {_id: 1, title: 1}}));

const lookupViewSearchExpected = [
    {"localField": "cakes", "weird": false, "cake_data": [{"ref_id": 1}]},
    {"localField": "cakes and kale", "weird": true, "cake_data": [{"ref_id": 8}]}
];
assert.sameMembers(lookupViewSearchExpected, lookupViewSearchCursor.toArray());

// $lookup (with $search) within $lookup.
const nestedLookupQuery = setupSearchQuery("cakes",
                                           1,
                                           [
                                               {_id: 1, $searchScore: 0.9},
                                               {_id: 2, $searchScore: 0.8},
                                               {_id: 5, $searchScore: 0.7},
                                               {_id: 6, $searchScore: 0.6},
                                               {_id: 8, $searchScore: 0.5}
                                           ],
                                           1);

const nestedInternalPipeline = makeLookupSearchPipeline(coll.getName(), nestedLookupQuery);
nestedInternalPipeline.push({$unwind: "$cake_data"},
                            {$match: {$expr: {$eq: ["$cake_data.ref_id", "$$local_ref_id"]}}});
const nestedLookupPipeline = [
    {
        $lookup: {
            from: collBase.getName(),
            let : {local_ref_id: "$ref_id"},
            pipeline: nestedInternalPipeline,
            as: "refs"
        }
    },
    {$unwind: "$refs"}
];

const nested = collBase2.aggregate(nestedLookupPipeline);
const nestedLookupExpected = [
    {
        "_id": 200,
        "ref_id": 1,
        "refs": {"localField": "cakes", "weird": false, "cake_data": {"ref_id": 1}}
    },
    {
        "_id": 201,
        "ref_id": 8,
        "refs": {"localField": "cakes and kale", "weird": true, "cake_data": {"ref_id": 8}}
    }
];
assert.sameMembers(nestedLookupExpected, nested.toArray());

// $lookup against non-trivial view($search) fails.

view1.drop();
assert.commandWorked(db.createView(view1.getName(), coll.getName(), [
    {$project: {"_id": 1}},
]));

assert.commandFailedWithCode(db.runCommand({
    aggregate: collBase.getName(),
    pipeline: makeLookupSearchPipeline(view1.getName(), {query: "cakes", path: "title"}),
    cursor: {}
}),
                             40602);

// $lookup against trivial view($search) works.
const lookupSearchViewQuery = setupSearchQuery("cakes",
                                               1,
                                               [
                                                   {_id: 1, $searchScore: 0.9},
                                                   {_id: 2, $searchScore: 0.8},
                                                   {_id: 5, $searchScore: 0.7},
                                                   {_id: 6, $searchScore: 0.6},
                                                   {_id: 8, $searchScore: 0.5}
                                               ],
                                               1);
view1.drop();
assert.commandWorked(db.createView(view1.getName(), coll.getName(), []));

assert.sameMembers(
    [
        {"localField": "cakes", "weird": false, "cake_data": [{"ref_id": 1}]},
        {"localField": "cakes and kale", "weird": true, "cake_data": [{"ref_id": 8}]}
    ],
    collBase.aggregate(makeLookupSearchPipeline(view1.getName(), lookupSearchViewQuery)).toArray());

// $unionWith against non-trivial view($search) fails.
view1.drop();
assert.commandWorked(db.createView(view1.getName(), coll.getName(), [
    {$project: {"_id": 1}},
]));

assert.commandFailedWithCode(db.runCommand({
    aggregate: collBase.getName(),
    pipeline: [
        {$project: {"localField": 1, "_id": 0}},
        {$unionWith: {coll: view1.getName(), pipeline: [{$search: unionSearchQuery}]}}
    ],
    cursor: {}
}),
                             40602);

// $unionWith against trivial view($search) passes.
const unionSearchViewQuery = setupSearchQuery("cakes",
                                              1,
                                              [
                                                  {_id: 1, $searchScore: 0.9},
                                                  {_id: 2, $searchScore: 0.8},
                                                  {_id: 5, $searchScore: 0.7},
                                                  {_id: 6, $searchScore: 0.6},
                                                  {_id: 8, $searchScore: 0.5}
                                              ],
                                              1);
view1.drop();
assert.commandWorked(db.createView(view1.getName(), coll.getName(), []));

assert.sameMembers(
    [
        {"localField": "cakes"},
        {"localField": "cakes and kale"},
        {"_id": 1, "title": "cakes"},
        {"_id": 2, "title": "cookies and cakes"},
        {"_id": 5, "title": "cakes and oranges"},
        {"_id": 6, "title": "cakes and apples"},
        {"_id": 8, "title": "cakes and kale"}
    ],
    collBase
        .aggregate([
            {$project: {"localField": 1, "_id": 0}},
            {$unionWith: {coll: view1.getName(), pipeline: [{$search: unionSearchViewQuery}]}}
        ])
        .toArray());

// Verify we fail if $lookup references "$$SEARCH_META" in its let variables, but we don't have
// "$$SEARCH_META" defined.
assert.commandFailedWithCode(db.runCommand({
    aggregate: collBase.getName(),
    pipeline: [
        {
            $lookup: {
                from: coll.getName(),
                let: {
                    // This should be detected and error.
                    myVar: {$mergeObjects: ["$$SEARCH_META", {distracting: "object"}]},
                },
                pipeline: [{$match: {$expr: {$eq: ["$$myVar", "$mySubObj"]}}}],
                as: "cake_data"
            }
        }
    ],
    cursor: {}
}),
                            6347902);

// Verify we can still succeed if $lookup references "$$SEARCH_META" in its let variables and we
// have "$$SEARCH_META" defined.
const successfulSearchThenMetaLookup = setupSearchQuery("cakes",
                                                        1,
                                                        [
                                                            {_id: 1, $searchScore: 0.9},
                                                            {_id: 2, $searchScore: 0.8},
                                                        ],
                                                        1);
assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$search: successfulSearchThenMetaLookup},
        {
            $lookup: {
                from: coll.getName(),
                let: {
                    // This should be detected and error.
                    myVar: {$mergeObjects: ["$$SEARCH_META", {distracting: "object"}]},
                },
                pipeline: [{$match: {$expr: {$eq: ["$$myVar", "$mySubObj"]}}}],
                as: "cake_data"
            }
        }
    ],
    cursor: {}
}));

// TODO SERVER-85637 Remove check for SearchExplainExecutionStats after the feature flag is removed.
if (checkSbeRestrictedOrFullyEnabled(db) &&
        FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'SearchInSbe') ||
    !FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'SearchExplainExecutionStats')) {
    jsTestLog(
        "Skipping explain tests with $lookup and $unionWith because it only applies to $search in classic engine with searchExplainExecStats enabled.");
    MongoRunner.stopMongod(conn);
    mongotmock.stop();
    quit();
}

// Test explain execution stats of $unionWith and $lookup with $search and $searchMeta.
for (const currentVerbosity of ["executionStats", "allPlansExecution"]) {
    const explainObj = {explain: "hello"};
    {
        const unionSearchQueryExplain = setupSearchQuery("cakes",
                                                         1,
                                                         [
                                                             {_id: 1, $searchScore: 0.9},
                                                             {_id: 2, $searchScore: 0.8},
                                                             {_id: 5, $searchScore: 0.7},
                                                             {_id: 6, $searchScore: 0.6},
                                                             {_id: 8, $searchScore: 0.5}
                                                         ],
                                                         1,
                                                         {verbosity: currentVerbosity},
                                                         explainObj);

        // $unionWith will run the search stage once to execute the query and again to obtain
        // execution stats, so we need to mock another response.

        setupSearchQuery("cakes",
                         1,
                         [
                             {_id: 1, $searchScore: 0.9},
                             {_id: 2, $searchScore: 0.8},
                             {_id: 5, $searchScore: 0.7},
                             {_id: 6, $searchScore: 0.6},
                             {_id: 8, $searchScore: 0.5}
                         ],
                         1,
                         {verbosity: currentVerbosity},
                         explainObj);

        const explainResult = collBase.explain(currentVerbosity).aggregate([
            {$project: {"localField": 1, "_id": 0}},
            {$unionWith: {coll: coll.getName(), pipeline: [{$search: unionSearchQueryExplain}]}}
        ]);
        const unionWithStage = getAggPlanStage(explainResult, "$unionWith");
        assert.neq(unionWithStage, null, explainResult);
        assert(unionWithStage.hasOwnProperty("nReturned"));
        assert.eq(NumberLong(7), unionWithStage["nReturned"]);

        let unionSubExplain = prepareUnionWithExplain(unionWithStage.$unionWith.pipeline);
        getMongotStagesAndValidateExplainExecutionStats({
            result: unionSubExplain,
            stageType: "$_internalSearchMongotRemote",
            verbosity: currentVerbosity,
            nReturned: NumberLong(5),
            explainObject: explainObj
        });
        getMongotStagesAndValidateExplainExecutionStats({
            result: unionSubExplain,
            stageType: "$_internalSearchIdLookup",
            verbosity: currentVerbosity,
            nReturned: NumberLong(5),
        });
    }
    {
        const unionSubSearchMetaExplain = setupSearchQuery("cakes",
                                                           1,
                                                           [
                                                               {_id: 1, $searchScore: 0.9},
                                                               {_id: 2, $searchScore: 0.8},
                                                           ],
                                                           "metaVal",
                                                           {verbosity: currentVerbosity},
                                                           explainObj);
        // $unionWith will run the search stage once to execute the query and again to obtain
        // execution stats, so we need to mock another response.
        setupSearchQuery("cakes",
                         1,
                         [
                             {_id: 1, $searchScore: 0.9},
                             {_id: 2, $searchScore: 0.8},
                         ],
                         "metaVal",
                         {verbosity: currentVerbosity},
                         explainObj);

        const explainResult = coll.explain(currentVerbosity).aggregate([
            {$match: {_id: 1}},
            {
                $unionWith:
                    {coll: coll.getName(), pipeline: [{$searchMeta: unionSubSearchMetaExplain}]}
            }
        ]);

        const unionWithStage = getAggPlanStage(explainResult, "$unionWith");
        assert.neq(unionWithStage, null, explainResult);
        assert(unionWithStage.hasOwnProperty("nReturned"));
        assert.eq(NumberLong(2), unionWithStage["nReturned"]);

        let unionSubExplain = prepareUnionWithExplain(unionWithStage.$unionWith.pipeline);
        getMongotStagesAndValidateExplainExecutionStats({
            result: unionSubExplain,
            stageType: "$searchMeta",
            verbosity: currentVerbosity,
            nReturned: NumberLong(1),
            explainObject: explainObj
        });
    }

    {
        const lookupSearchQueryExplain = setupSearchQuery("cakes",
                                                          1,
                                                          [
                                                              {_id: 1, $searchScore: 0.9},
                                                              {_id: 2, $searchScore: 0.8},
                                                              {_id: 5, $searchScore: 0.7},
                                                              {_id: 6, $searchScore: 0.6},
                                                              {_id: 8, $searchScore: 0.5}
                                                          ],
                                                          1,
                                                          {verbosity: currentVerbosity},
                                                          explainObj);

        const explainResult =
            collBase.explain(currentVerbosity)
                .aggregate(makeLookupSearchPipeline(
                    coll.getName(), lookupSearchQueryExplain, false /*localField*/));
        // $lookup doesn't include the stats of the stages in its subpipeline, so the search
        // explain results cannot be checked. We check that the lookup stage returned results.
        const lookupStage = getAggPlanStage(explainResult, "$lookup");
        assert.neq(lookupStage, null, explainResult);
        assert(lookupStage.hasOwnProperty("nReturned"));
        assert.eq(NumberLong(2), lookupStage["nReturned"]);
        assert(explainResult.stages[0].$cursor.hasOwnProperty("executionStats"));
    }

    {
        const lookupSearchMetaExplain = setupSearchQuery("cakes",
                                                         1,
                                                         [
                                                             {_id: 1, $searchScore: 0.9},
                                                             {_id: 2, $searchScore: 0.8},
                                                         ],
                                                         "metaVal",
                                                         {verbosity: currentVerbosity},
                                                         explainObj);

        const explainResult = coll.explain(currentVerbosity).aggregate([{$match: {_id: 1}}, // Match one document.
                                                                  {
                                                                  $lookup: {
                                                                  from: coll.getName(),
                                                                  pipeline: [{$searchMeta: lookupSearchMetaExplain}],
                                                                  as: "arr",
                                                                  }
                                                                  },
                                                                  ]);
        // $lookup doesn't include the stats of the stages in its subpipeline, so the search
        // explain results cannot be checked. We check that the lookup stage returned results.
        const lookupStage = getAggPlanStage(explainResult, "$lookup");
        assert.neq(lookupStage, null, explainResult);
        assert(lookupStage.hasOwnProperty("nReturned"));
        assert.eq(NumberLong(1), lookupStage["nReturned"]);
        assert(explainResult.stages[0].$cursor.hasOwnProperty("executionStats"));
    }
}

MongoRunner.stopMongod(conn);
mongotmock.stop();
