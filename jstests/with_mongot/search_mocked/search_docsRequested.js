/**
 * Tests that if a query has an extractable limit, we send a search command to mongot with that
 * information in the docsRequested field.
 * All tests are skipped if featureFlagSearchBatchSizeTuning is enabled, since this file only tests
 * the docsRequested options, whereas that flag enables the batchSize option.
 * TODO SERVER-92576 Remove this test when featureFlagSearchBatchSizeLimit is removed.
 * @tags: [requires_fcv_71]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/sbe_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    getDefaultProtocolVersionForPlanShardedSearch,
    mockPlanShardedSearchResponse,
    mongotCommandForQuery,
    MongotMock
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = "test";
const collName = "search_docsrequested";
const chunkBoundary = 8;
const protocolVersion = getDefaultProtocolVersionForPlanShardedSearch();

const docs = [
    {"_id": 1, "title": "cakes"},
    {"_id": 2, "title": "cookies and cakes"},
    {"_id": 3, "title": "vegetables"},
    {"_id": 4, "title": "oranges"},
    {"_id": 5, "title": "cakes and oranges"},
    {"_id": 6, "title": "cakes and apples"},
    {"_id": 7, "title": "apples"},
    {"_id": 8, "title": "cakes and xyz"},
    {"_id": 9, "title": "cakes and blueberries"},
    {"_id": 10, "title": "cakes and strawberries"},
    {"_id": 11, "title": "cakes and raspberries"},
    {"_id": 12, "title": "cakes and cakes"},
    {"_id": 13, "title": "cakes and elderberries"},
    {"_id": 14, "title": "cakes and carrots"},
    {"_id": 15, "title": "cakes and more cakes"},
    {"_id": 16, "title": "cakes and even more cakes"},
];

const foreignCollectionDocs = [
    {"_id": 1, "fruit": "raspberries"},
    {"_id": 2, "fruit": "blueberries"},
    {"_id": 3, "fruit": "strawberries"},
    {"_id": 4, "fruit": "gooseberries"},
    {"_id": 5, "fruit": "mangos"},
];
const foreignCollName = "fruits";
const foreignChunkBoundary = 3;

const searchQuery = {
    query: "cakes",
    path: "title"
};

// All the documents that would be returned by the search query above.
let relevantDocs = [];
let relevantSearchDocs = [];
let relevantSearchDocsShard0 = [];
let relevantSearchDocsShard1 = [];
let searchScore = 0.300;
for (let i = 0; i < docs.length; i++) {
    if (docs[i]["title"].includes(searchQuery.query)) {
        relevantDocs.push(docs[i]);

        // Standalone case.
        relevantSearchDocs.push({_id: docs[i]._id, $searchScore: searchScore});

        // Sharded environment case.
        if (docs[i]._id < chunkBoundary) {
            relevantSearchDocsShard0.push({_id: docs[i]._id, $searchScore: searchScore});
        } else {
            relevantSearchDocsShard1.push({_id: docs[i]._id, $searchScore: searchScore});
        }

        // The documents with lower _id will have a higher search score.
        searchScore = searchScore - 0.001;
    }
}
assert.eq(13, relevantSearchDocs.length);
assert.eq(4, relevantSearchDocsShard0.length);
assert.eq(9, relevantSearchDocsShard1.length);

// Mongot may return slightly more documents than mongod requests as an optimization for the case
// when $idLookup filters out some of them.
function calcNumDocsMongotShouldReturn(extractedLimit) {
    return Math.max(Math.ceil(1.064 * extractedLimit), 10);
}

function buildHistoryStandalone(coll, collUUID, extractedLimit, mongotConn) {
    let mongotReturnedDocs = calcNumDocsMongotShouldReturn(extractedLimit);
    {
        const cursorId = NumberLong(123);
        const history = [
            {
                expectedCommand: mongotCommandForQuery({
                    query: searchQuery,
                    collName: collName,
                    db: dbName,
                    collectionUUID: collUUID,
                    cursorOptions: {docsRequested: NumberInt(extractedLimit)}
                }),
                response: {
                    cursor: {
                        id: NumberLong(0),
                        ns: coll.getFullName(),
                        nextBatch: relevantSearchDocs.slice(0, mongotReturnedDocs),
                    },
                    ok: 1
                }
            },
        ];
        assert.commandWorked(
            mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
    }
}

function buildHistoryShardedEnv(coll, collUUID, extractedLimit, stWithMock) {
    let mongotReturnedDocs = calcNumDocsMongotShouldReturn(extractedLimit);
    {
        const cursorId = NumberLong(123);
        const metaId = NumberLong(2);

        // Set history for shard 0.
        const history0 = [
            {
                expectedCommand: mongotCommandForQuery({
                    query: searchQuery,
                    collName: collName,
                    db: dbName,
                    collectionUUID: collUUID,
                    protocolVersion: protocolVersion,
                    cursorOptions: {docsRequested: NumberInt(extractedLimit)}
                }),
                response: {
                    cursors: [
                        {
                            cursor: {
                                id: NumberLong(0),
                                type: "results",
                                ns: coll.getFullName(),
                                nextBatch: relevantSearchDocsShard0.slice(0, mongotReturnedDocs),
                            },
                            ok: 1
                        },
                        {
                            cursor: {
                                id: NumberLong(0),
                                ns: coll.getFullName(),
                                type: "meta",
                                nextBatch: [{metaVal: 1}],
                            },
                            ok: 1
                        }
                    ]

                }
            },
        ];
        const s0Mongot = stWithMock.getMockConnectedToHost(stWithMock.st.rs0.getPrimary());
        s0Mongot.setMockResponses(history0, cursorId, metaId);

        // Set history for shard 1.
        const history1 = [
            {
                expectedCommand: mongotCommandForQuery({
                    query: searchQuery,
                    collName: collName,
                    db: dbName,
                    collectionUUID: collUUID,
                    protocolVersion: protocolVersion,
                    cursorOptions: {docsRequested: NumberInt(extractedLimit)}
                }),
                response: {
                    cursors: [
                        {
                            cursor: {
                                id: NumberLong(0),
                                type: "results",
                                ns: coll.getFullName(),
                                nextBatch: relevantSearchDocsShard1.slice(0, mongotReturnedDocs),
                            },
                            ok: 1
                        },
                        {
                            cursor: {
                                id: NumberLong(0),
                                ns: coll.getFullName(),
                                type: "meta",
                                nextBatch: [{metaVal: 2}],
                            },
                            ok: 1
                        }
                    ]
                }
            },
        ];
        const s1Mongot = stWithMock.getMockConnectedToHost(stWithMock.st.rs1.getPrimary());
        s1Mongot.setMockResponses(history1, cursorId, metaId);

        mockPlanShardedSearchResponse(
            collName, searchQuery, dbName, undefined /*sortSpec*/, stWithMock);
    }
}

function runAndAssert(
    pipeline, extractedLimit, expectedResults, coll, collUUID, standaloneConn, stConn) {
    // Only one of standaloneConn and stConn can be non-null.
    if (standaloneConn != null) {
        assert(stConn == null);
        buildHistoryStandalone(coll, collUUID, extractedLimit, standaloneConn);
    } else {
        assert(standaloneConn == null);
        buildHistoryShardedEnv(coll, collUUID, extractedLimit, stConn);
    }
    let cursor = coll.aggregate(pipeline);
    assert.eq(expectedResults, cursor.toArray());
}

// The extractable limit optimization cannot be done if there is a stage between $search and
// $limit that would change the number of documents, such as $match. Thus, there should be no
// 'docsRequested' field in the command sent to mongot in this case.
function expectNoDocsRequestedInCommand(coll, collUUID, mongotConn, stWithMock) {
    let pipeline = [{$search: searchQuery}, {$match: {title: {$regex: "more cakes"}}}, {$limit: 1}];
    let cursorId = NumberLong(123);

    // Only one of mongotConn and stWithMock can be non-null.
    if (mongotConn != null) {
        assert(stWithMock == null);
        const history = [
            {
                expectedCommand: {
                    search: coll.getName(),
                    collectionUUID: collUUID,
                    query: searchQuery,
                    $db: dbName,
                },
                response: {
                    cursor: {
                        id: NumberLong(0),
                        ns: coll.getFullName(),
                        nextBatch: [
                            {_id: 15, $searchScore: 0.789},
                            {_id: 16, $searchScore: 0.123},
                        ]
                    },
                    ok: 1
                }
            },
        ];
        assert.commandWorked(
            mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
    } else {
        assert(mongotConn == null);
        const metaId = NumberLong(2);
        // Set history for shard 0.
        const history0 = [
            {
                expectedCommand: {
                    search: coll.getName(),
                    collectionUUID: collUUID,
                    query: searchQuery,
                    $db: dbName,
                    intermediate: protocolVersion
                },
                response: {
                    cursors: [
                        {
                            cursor: {
                                id: NumberLong(0),
                                type: "results",
                                ns: coll.getFullName(),
                                nextBatch: [],
                            },
                            ok: 1
                        },
                        {
                            cursor: {
                                id: NumberLong(0),
                                ns: coll.getFullName(),
                                type: "meta",
                                nextBatch: [{metaVal: 1}],
                            },
                            ok: 1
                        }
                    ]
                }
            },
        ];
        const s0Mongot = stWithMock.getMockConnectedToHost(stWithMock.st.rs0.getPrimary());
        s0Mongot.setMockResponses(history0, cursorId, metaId);

        // Set history for shard 1.
        const history1 = [
            {
                expectedCommand: {
                    search: coll.getName(),
                    collectionUUID: collUUID,
                    query: searchQuery,
                    $db: dbName,
                    intermediate: protocolVersion
                },
                response: {
                    cursors: [
                        {
                            cursor: {
                                id: NumberLong(0),
                                type: "results",
                                ns: coll.getFullName(),
                                nextBatch: [
                                    {_id: 15, $searchScore: 0.789},
                                    {_id: 16, $searchScore: 0.123},
                                ]
                            },
                            ok: 1
                        },
                        {
                            cursor: {
                                id: NumberLong(0),
                                ns: coll.getFullName(),
                                type: "meta",
                                nextBatch: [{metaVal: 1}],
                            },
                            ok: 1
                        }
                    ]
                }
            },
        ];
        const s1Mongot = stWithMock.getMockConnectedToHost(stWithMock.st.rs1.getPrimary());
        s1Mongot.setMockResponses(history1, cursorId, metaId);

        mockPlanShardedSearchResponse(
            collName, searchQuery, dbName, undefined /*sortSpec*/, stWithMock);
    }
    let cursor = coll.aggregate(pipeline);
    const expected = [
        {"_id": 15, "title": "cakes and more cakes"},
    ];
    assert.eq(expected, cursor.toArray());
}

// Perform a $search query where $$SEARCH_META is referenced after a $limit stage.
function searchMetaAfterLimit(coll, collUUID, stWithMock) {
    let st = stWithMock.st;
    let limit = 3;
    let mongotReturnedDocs = calcNumDocsMongotShouldReturn(limit);

    let pipeline =
        [{$search: searchQuery}, {$limit: limit}, {$project: {_id: 1, meta: "$$SEARCH_META"}}];
    let expected = relevantDocs.slice(0, limit);
    // Modify the expected documents to reflect the $project stage in the pipeline.
    for (let i = 0; i < expected.length; i++) {
        expected[i] = {_id: expected[i]["_id"], meta: {value: 0}};
    }

    // Set history for shard 0.
    {
        const resultsID = NumberLong(11);
        const metaID = NumberLong(12);
        const historyResults = [
            {
                expectedCommand: mongotCommandForQuery({
                    query: searchQuery,
                    collName: collName,
                    db: dbName,
                    collectionUUID: collUUID,
                    protocolVersion: protocolVersion,
                    cursorOptions: {docsRequested: NumberInt(limit)}
                }),
                response: {
                    ok: 1,
                    cursors: [
                        {
                            cursor: {
                                id: NumberLong(0),
                                type: "results",
                                ns: coll.getFullName(),
                                nextBatch: relevantSearchDocsShard0.slice(0, mongotReturnedDocs)
                            },
                            ok: 1
                        },
                        {
                            cursor: {
                                id: NumberLong(0),
                                ns: coll.getFullName(),
                                type: "meta",
                                nextBatch: [{value: 0}],
                            },
                            ok: 1
                        }
                    ]
                }
            },
        ];
        const mongot = stWithMock.getMockConnectedToHost(st.rs0.getPrimary());
        mongot.setMockResponses(historyResults, resultsID, metaID);
    }

    // Set history for shard 1.
    {
        const resultsID = NumberLong(21);
        const metaID = NumberLong(22);
        const historyResults = [
            {
                expectedCommand: mongotCommandForQuery({
                    query: searchQuery,
                    collName: collName,
                    db: dbName,
                    collectionUUID: collUUID,
                    protocolVersion: protocolVersion,
                    cursorOptions: {docsRequested: NumberInt(limit)}
                }),
                response: {
                    ok: 1,
                    cursors: [
                        {
                            cursor: {
                                id: NumberLong(0),
                                type: "results",
                                ns: coll.getFullName(),
                                nextBatch: relevantSearchDocsShard1.slice(0, mongotReturnedDocs)

                            },
                            ok: 1
                        },
                        {
                            cursor: {
                                id: NumberLong(0),
                                ns: coll.getFullName(),
                                type: "meta",
                                nextBatch: [{value: 0}],
                            },
                            ok: 1
                        }
                    ]
                }
            },
        ];
        const mongot = stWithMock.getMockConnectedToHost(st.rs1.getPrimary());
        mongot.setMockResponses(historyResults, resultsID, metaID);
    }

    // Set history for mongos.
    {
        const mergingPipelineHistory = [{
            expectedCommand: {
                planShardedSearch: collName,
                query: searchQuery,
                $db: dbName,
                searchFeatures: {shardedSort: 1}
            },
            response: {
                ok: 1,
                protocolVersion: NumberInt(1),
                // This does not represent an actual merging pipeline. The merging pipeline is
                // arbitrary, it just must only generate one document.
                metaPipeline: [{$limit: 1}]
            }
        }];
        const mongot = stWithMock.getMockConnectedToHost(stWithMock.st.s);
        mongot.setMockResponses(mergingPipelineHistory, 1);
    }

    let cursor = coll.aggregate(pipeline);
    assert.eq(expected, cursor.toArray());
}

function buildHistorySearchWithinLookupStandalone(db, mongotConn, searchLookupQuery, numBerries) {
    let foreignColl = db.getCollection(foreignCollName);
    assert.commandWorked(foreignColl.insertMany(foreignCollectionDocs));
    let foreignCollUUID = getUUIDFromListCollections(db, foreignCollName);

    {
        const history = [
            {
                expectedCommand: {
                    search: foreignCollName,
                    collectionUUID: foreignCollUUID,
                    query: searchLookupQuery,
                    $db: dbName,
                    cursorOptions: {docsRequested: numBerries},
                },
                response: {
                    cursor: {
                        id: NumberLong(0),
                        ns: foreignColl.getFullName(),
                        nextBatch: [
                            {"_id": 1, "$searchScore": 0.300},
                            {"_id": 2, "$searchScore": 0.299},
                            {"_id": 3, "$searchScore": 0.298},
                            {"_id": 4, "$searchScore": 0.297},
                            // We set mongotmock to return 4 documents here because of the
                            // oversubscription that mongot would do (mongot would want to return 10
                            // documents in this case because 3 * 1.064 < 10, but there are only 4
                            // that satisfy the query so we return those 4).
                        ],
                    },
                    ok: 1
                }
            },
        ];
        // Only one response is needed, as $lookup executes $search once and caches the response.
        assert.commandWorked(mongotConn.adminCommand(
            {setMockResponses: 1, cursorId: NumberLong(123), history: history}));
    }
}

function buildHistorySearchWithinLookupShardedEnv(db, stWithMock, searchLookupQuery, numBerries) {
    let st = stWithMock.st;

    let foreignColl = db.getCollection(foreignCollName);
    assert.commandWorked(foreignColl.insertMany(foreignCollectionDocs));
    let foreignCollUUID =
        getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), foreignCollName);

    // Shard the foreign collection for the $lookup test and move the higher chunk to shard1.
    st.shardColl(
        foreignColl, {_id: 1}, {_id: foreignChunkBoundary}, {_id: foreignChunkBoundary + 1});

    const planShardedSearchHistory = [{
        expectedCommand: {
            planShardedSearch: foreignCollName,
            query: searchLookupQuery,
            $db: dbName,
            searchFeatures: {shardedSort: 1}
        },
        response: {ok: 1, protocolVersion: NumberInt(1), metaPipeline: [{$limit: 1}]}
    }];

    function history(cursorId, docsToReturn) {
        // We will set mongotmock to return 2 of the documents given in the first batch, and the
        // rest as a response to the expected getMore.
        let docsInFirstBatch = 2;
        assert(docsToReturn.length >= docsInFirstBatch);

        return [
            {
                expectedCommand: {
                    search: foreignCollName,
                    collectionUUID: foreignCollUUID,
                    query: searchLookupQuery,
                    $db: dbName,
                    cursorOptions: {docsRequested: numBerries},
                    intermediate: protocolVersion
                },
                response: {
                    cursors: [
                        {
                            cursor: {
                                id: NumberLong(cursorId),
                                type: "results",
                                ns: foreignColl.getFullName(),
                                nextBatch: docsToReturn.slice(0, 2),
                            },
                            ok: 1
                        },
                        {
                            cursor: {
                                id: NumberLong(0),
                                ns: foreignColl.getFullName(),
                                type: "meta",
                                nextBatch: [{metaVal: 1}],
                            },
                            ok: 1
                        }
                    ]
                }
            },
            {
                expectedCommand: {
                    getMore: NumberLong(cursorId),
                    collection: foreignCollName,
                    cursorOptions: {docsRequested: numBerries - docsInFirstBatch}
                },
                response: {
                    cursor: {
                        id: NumberLong(0),
                        ns: foreignColl.getFullName(),
                        nextBatch: docsToReturn.slice(2),
                    },
                    ok: 1
                }
            },

        ];
    }

    const s0Mongot = stWithMock.getMockConnectedToHost(stWithMock.st.rs0.getPrimary());
    const s1Mongot = stWithMock.getMockConnectedToHost(stWithMock.st.rs1.getPrimary());

    // We need a new cursorId for each setMockResponses below because calling setMockResponses with
    // the same cursorId twice overwrites the first mock response.
    let cursorId = 123;
    let metaId = 2;

    // Each shard will invoke PSS during execution of $search in $lookup. Only one response is
    // necessary since $search is executed once and cached.

    // Mock the responses for the commands resulting from parsing the local collection documents on
    // shard0.
    s0Mongot.setMockResponses(planShardedSearchHistory, NumberLong(cursorId++));

    // Mock the responses for the commands resulting from parsing the local collection
    // documents on shard1.
    s1Mongot.setMockResponses(planShardedSearchHistory, NumberLong(cursorId++));

    // As part of $lookup execution, each shard will send a search command to itself and to the
    // other shard. Only occurs once per shard as $search is executed once and cached.
    for (let i = 0; i < 2; i++) {
        s0Mongot.setMockResponses(
            history(cursorId, [{_id: 1, $searchScore: 0.3}, {_id: 2, $searchScore: 0.299}]),
            NumberLong(cursorId++),
            NumberLong(metaId++));

        s1Mongot.setMockResponses(
            history(cursorId, [{_id: 3, $searchScore: 0.298}, {_id: 4, $searchScore: 0.297}]),
            NumberLong(cursorId++),
            NumberLong(metaId++));
    }

    stWithMock.getMockConnectedToHost(stWithMock.st.rs0.getPrimary()).disableOrderCheck();
    stWithMock.getMockConnectedToHost(stWithMock.st.rs1.getPrimary()).disableOrderCheck();
}

function testSearchWithinLookup(db, coll, mongotConn, stWithMock) {
    // The $lookup subpipeline produces one document of the form {three_berries: {berries: [...]}}
    // where the contents of the array are the first 3 fruits (3 from the limit stage) in the
    // foreign collection documents that contain the substring "berries".
    // Each document in the local collection has this document produced from the subpipeline after
    // the $lookup stage. The $unwind stages unwind the array in that document. The $match stage
    // filters out documents where the title field does not contain any of the first 3 berries as a
    // substring. Thus, we are left with documents in the local collection where the titles contain
    // one of the first 3 berries from the foreign collection documents.
    const searchLookupQuery = {query: "berries", path: "fruit"};
    const numBerries = 3;
    const expected = [
        {"title": "cakes and blueberries"},
        {"title": "cakes and strawberries"},
        {"title": "cakes and raspberries"},
    ];
    const pipeline = [
        {
            $lookup: {
                from: foreignCollName,
                pipeline: [
                    {$search: searchLookupQuery},
                    {$limit: numBerries},
                    {$group: {_id: null, berries: {$addToSet: "$fruit"}}},
                    {$project: {_id: 0}}
                ],
                as: "three_berries"
            }
        },
        {$unwind: {path: "$three_berries"}},
        {$unwind: {path: "$three_berries.berries"}},
        {$match: {$expr: {$ne: [{$indexOfCP: ["$title", "$three_berries.berries"]}, -1]}}},
        {$project: {_id: 0, title: 1}}
    ];

    // Exactly one of mongotConn and stWithMock is expected to be null.
    if (mongotConn != null) {
        assert(stWithMock == null);
        buildHistorySearchWithinLookupStandalone(db, mongotConn, searchLookupQuery, numBerries);
    } else {
        assert(mongotConn == null);
        buildHistorySearchWithinLookupShardedEnv(db, stWithMock, searchLookupQuery, numBerries);
    }

    let cursor = coll.aggregate(pipeline);
    assert.eq(expected, cursor.toArray());
}

// Perform a $search query where a getMore is required.
function getMoreCaseBuildHistoryStandalone(coll, collUUID, mongotConn, limitVal, orphanDocs) {
    const cursorId = NumberLong(123);

    // This tests that mongod will getMore thrice as necessary to obtain all the relevent documents.
    // There are 13 total documents that should be returned by the search query and the limit in the
    // query is 15. We define the first batch returned by mongot to be 10 of the documents actually
    // in the collection and 6 documents that will not go through the $idLookup stage because they
    // don't exist in the collection (this simulates the case where the mongot index of the data is
    // stale and some documents have been deleted from the collection but the index has not yet been
    // updated to reflect this). 6 comes from the oversubscription that mongot will do (limitVal
    // * 1.064, rounded up, minus the 10 real documents). This will cause mongod to getMore with 5
    // documents since only 10 in the first batch were valid. To this, mongot will return 10
    // documents (as that is the minumum number of documents mongot returns for a query with an
    // extractable limit), but all of these will be more orphan documents that aren't in the
    // collection. mongod will send another getMore for 5 documents and to this mongot will return a
    // batch with only 1 valid document (to exercise the case where mongot returns fewer documents
    // than requested, but there are still more to be returned. This could happen if the batch
    // exceeds the 16MB limit.) mongod will send another getMore for 4 documents and to this mongot
    // will send the remaining 2 documents in the collection that satisfy the search query.
    const batch1 = relevantSearchDocs.slice(0, 10).concat(orphanDocs.slice(0, 6));
    const batch2 = orphanDocs.slice(6);
    const batch3 = relevantSearchDocs.slice(10, 11);
    const batch4 = relevantSearchDocs.slice(11);

    const history = [
        {
            expectedCommand: mongotCommandForQuery({
                query: searchQuery,
                collName: collName,
                db: dbName,
                collectionUUID: collUUID,
                cursorOptions: {docsRequested: NumberInt(limitVal)}
            }),
            response: {
                cursor: {
                    id: cursorId,
                    ns: coll.getFullName(),
                    nextBatch: batch1,
                },
                ok: 1
            }
        },
        {
            expectedCommand:
                {getMore: cursorId, collection: coll.getName(), cursorOptions: {docsRequested: 5}},
            response: {
                ok: 1,
                cursor: {
                    id: cursorId,
                    ns: coll.getFullName(),
                    nextBatch: batch2,
                },
            }
        },
        {
            expectedCommand:
                {getMore: cursorId, collection: coll.getName(), cursorOptions: {docsRequested: 5}},
            response: {
                ok: 1,
                cursor: {
                    id: cursorId,
                    ns: coll.getFullName(),
                    nextBatch: batch3,
                },
            }
        },
        {
            expectedCommand:
                {getMore: cursorId, collection: coll.getName(), cursorOptions: {docsRequested: 4}},
            response: {
                ok: 1,
                cursor: {
                    id: NumberLong(0),  // We have exhausted the cursor.
                    ns: coll.getFullName(),
                    nextBatch: batch4,
                },
            }
        },
    ];
    assert.commandWorked(
        mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
}

function getMoreCaseBuildHistoryShardedEnv(coll, collUUID, stWithMock, limitVal, orphanDocs) {
    const cursorId = NumberLong(123);
    const metaId = NumberLong(2);

    // This is a similar situation to the standlone case.

    // For the first batch, mongot will return 10 total real documents to the shards (3 for shard0
    // and 7 for shard1) and 6 total documents that are not in the collection (3 for each shard).
    const batch1shard0 = relevantSearchDocsShard0.slice(0, 3).concat(orphanDocs.slice(0, 3));
    const batch1shard1 = relevantSearchDocsShard1.slice(0, 7).concat(orphanDocs.slice(3, 6));

    // The amount of documents that each shard will request in the getMore will be the difference
    // between the limit in the pipeline and the number of valid documents each shard got back from
    // the first batch.
    const docsRequestedShard0 = limitVal - 3;
    const docsRequestedShard1 = limitVal - 7;

    // For the second batch, mongot will return 5 orphan documents to each shard.
    const batch2shard0 = orphanDocs.slice(6, 11);
    const batch2shard1 = orphanDocs.slice(11);

    // For the third batch, mongot will return 1 of the documents on shard 1.
    const batch3shard0 = [];
    const batch3shard1 = relevantSearchDocsShard1.slice(7, 8);

    // For the third batch, mongot will return the last remaining document on shard0 and the last
    // remaining document on shard1.
    const batch4shard0 = relevantSearchDocsShard0.slice(3);
    const batch4shard1 = relevantSearchDocsShard1.slice(8);

    // Set history for shard 0.
    const history0 = [
        {
            expectedCommand: mongotCommandForQuery({
                query: searchQuery,
                collName: collName,
                db: dbName,
                collectionUUID: collUUID,
                protocolVersion: protocolVersion,
                cursorOptions: {docsRequested: limitVal}
            }),
            response: {
                cursors: [
                    {
                        cursor: {
                            id: cursorId,
                            type: "results",
                            ns: coll.getFullName(),
                            nextBatch: batch1shard0,
                        },
                        ok: 1
                    },
                    {
                        cursor: {
                            id: NumberLong(0),
                            ns: coll.getFullName(),
                            type: "meta",
                            nextBatch: [{metaVal: 1}],
                        },
                        ok: 1
                    }
                ]
            }
        },
        {
            expectedCommand: {
                getMore: cursorId,
                collection: coll.getName(),
                cursorOptions: {docsRequested: docsRequestedShard0}
            },
            response: {
                ok: 1,
                cursor: {
                    id: cursorId,
                    ns: coll.getFullName(),
                    nextBatch: batch2shard0,
                },
            }
        },
        {
            expectedCommand: {
                getMore: cursorId,
                collection: coll.getName(),
                cursorOptions: {docsRequested: docsRequestedShard0}
            },
            response: {
                ok: 1,
                cursor: {
                    id: cursorId,
                    ns: coll.getFullName(),
                    nextBatch: batch3shard0,
                },
            }
        },
        {
            expectedCommand: {
                getMore: cursorId,
                collection: coll.getName(),
                // Since the previous batch returned no documents for this shard, this docsRequested
                // value will be the same as the previous.
                cursorOptions: {docsRequested: docsRequestedShard0}
            },
            response: {
                ok: 1,
                cursor: {
                    id: NumberLong(0),  // We have exhausted the cursor.
                    ns: coll.getFullName(),
                    nextBatch: batch4shard0,
                },
            }
        },
    ];
    const s0Mongot = stWithMock.getMockConnectedToHost(stWithMock.st.rs0.getPrimary());
    s0Mongot.setMockResponses(history0, cursorId, metaId);

    // Set history for shard 1.
    const history1 = [
        {
            expectedCommand: mongotCommandForQuery({
                query: searchQuery,
                collName: collName,
                db: dbName,
                collectionUUID: collUUID,
                protocolVersion: protocolVersion,
                cursorOptions: {docsRequested: NumberInt(limitVal)}
            }),
            response: {
                cursors: [
                    {
                        cursor: {
                            id: cursorId,
                            type: "results",
                            ns: coll.getFullName(),
                            nextBatch: batch1shard1,
                        },
                        ok: 1
                    },
                    {
                        cursor: {
                            id: NumberLong(0),
                            ns: coll.getFullName(),
                            type: "meta",
                            nextBatch: [{metaVal: 1}],
                        },
                        ok: 1
                    }
                ]

            }
        },
        {
            expectedCommand: {
                getMore: cursorId,
                collection: coll.getName(),
                cursorOptions: {docsRequested: docsRequestedShard1}
            },
            response: {
                ok: 1,
                cursor: {
                    id: cursorId,
                    ns: coll.getFullName(),
                    nextBatch: batch2shard1,
                },
            }
        },
        {
            expectedCommand: {
                getMore: cursorId,
                collection: coll.getName(),
                cursorOptions: {docsRequested: docsRequestedShard1}
            },
            response: {
                ok: 1,
                cursor: {
                    id: cursorId,
                    ns: coll.getFullName(),
                    nextBatch: batch3shard1,
                },
            }
        },
        {
            expectedCommand: {
                getMore: cursorId,
                collection: coll.getName(),
                // Since the previous batch returned one valid document for this shard, this
                // docsRequested value will be one less than the previous.
                cursorOptions: {docsRequested: docsRequestedShard1 - 1}
            },
            response: {
                ok: 1,
                cursor: {
                    id: NumberLong(0),  // We have exhausted the cursor.
                    ns: coll.getFullName(),
                    nextBatch: batch4shard1,
                },
            }
        },
    ];
    const s1Mongot = stWithMock.getMockConnectedToHost(stWithMock.st.rs1.getPrimary());
    s1Mongot.setMockResponses(history1, cursorId, metaId);

    mockPlanShardedSearchResponse(
        collName, searchQuery, dbName, undefined /*sortSpec*/, stWithMock);
}

// Perform a $search query where a getMore is required.
function getMoreCase(coll, collUUID, standaloneConn, stConn) {
    const limitVal = 15;
    const pipeline = [{$search: searchQuery}, {$limit: limitVal}];

    // Construct 16 fake documents that aren't in the collection for mongot to return (mimicking an
    // out-of-date index) that will not pass the idLookup stage.
    let orphanDocs = [];
    for (let i = 20; i < 36; i++) {
        orphanDocs.push({_id: i, $searchScore: 0.3});
    }

    // Exactly one of standaloneConn and stConn is execpted to be null.
    if (standaloneConn != null) {
        assert(stConn == null);
        getMoreCaseBuildHistoryStandalone(coll, collUUID, standaloneConn, limitVal, orphanDocs);
    } else {
        assert(standaloneConn == null);
        getMoreCaseBuildHistoryShardedEnv(coll, collUUID, stConn, limitVal, orphanDocs);
    }

    let cursor = coll.aggregate(pipeline);
    // All relevant documents are expected to be returned by the query.
    assert.eq(relevantDocs, cursor.toArray());
}

function runTest(db, collUUID, standaloneConn, stConn) {
    let coll = db.getCollection(collName);
    function runSearchQueries(limitVal, otherLimitVal, skipVal) {
        // Perform a $search query with a limit stage.
        let pipeline = [{$search: searchQuery}, {$limit: limitVal}];
        // The extracted limit here comes from the limit value in the pipeline.
        let expected = relevantDocs.slice(0, limitVal);
        runAndAssert(pipeline, limitVal, expected, coll, collUUID, standaloneConn, stConn);

        // Perform a $search query with a $skip followed by $limit.
        pipeline = [{$search: searchQuery}, {$skip: skipVal}, {$limit: limitVal}];
        // The extracted limit here comes from the sum of the limit and skip values in the pipeline.
        expected = relevantDocs.slice(skipVal).slice(0, limitVal);
        runAndAssert(
            pipeline, limitVal + skipVal, expected, coll, collUUID, standaloneConn, stConn);

        // Perform a $search query with multiple limit stages.
        pipeline = [{$search: searchQuery}, {$limit: limitVal}, {$limit: otherLimitVal}];
        // The extracted limit here comes from the minimum of the two limit values in the pipeline.
        expected = relevantDocs.slice(0, Math.min(limitVal, otherLimitVal));
        runAndAssert(pipeline,
                     Math.min(limitVal, otherLimitVal),
                     expected,
                     coll,
                     collUUID,
                     standaloneConn,
                     stConn);

        // Perform a $search query with a limit and multiple skip stages.
        pipeline = [{$search: searchQuery}, {$skip: skipVal}, {$skip: skipVal}, {$limit: limitVal}];
        // The extracted limit here comes from the value of the limit plus the values of the two
        // skip stages in the pipeline.
        expected = relevantDocs.slice(skipVal + skipVal).slice(0, limitVal);
        runAndAssert(pipeline,
                     skipVal + skipVal + limitVal,
                     expected,
                     coll,
                     collUUID,
                     standaloneConn,
                     stConn);

        // Perform a $search query with multiple limit stages and multiple skip stages.
        pipeline = [
            {$search: searchQuery},
            {$skip: skipVal},
            {$skip: skipVal},
            {$limit: limitVal},
            {$limit: otherLimitVal}
        ];
        // The extracted limit here comes from the minimum of the two limit values plus the values
        // of the two skip stages in the pipeline.
        expected =
            relevantDocs.slice(skipVal + skipVal).slice(0, Math.min(limitVal, otherLimitVal));
        runAndAssert(pipeline,
                     skipVal + skipVal + Math.min(limitVal, otherLimitVal),
                     expected,
                     coll,
                     collUUID,
                     standaloneConn,
                     stConn);
    }

    // Run the search queries with limit and skip values such that mongod will extract a user limit
    // of less than 10, which means we will exercise the branch where mongot returns a minimum of 10
    // documents.
    runSearchQueries(3 /* limitVal */, 2 /* otherLimitVal */, 1 /* skipVal */);

    // Run the search queries with limit and skip values such that mongod will extract a user limit
    // of greater than 10, which means we will exercise the branch where mongot the extracted limit
    // multiplied by the oversubscription factor.
    runSearchQueries(11 /* limitVal */, 10 /* otherLimitVal */, 1 /* skipVal */);

    expectNoDocsRequestedInCommand(coll, collUUID, standaloneConn, stConn);

    // SERVER-80648 $search in SBE doesn't support the batch size optimization, so skip the tests.
    if (!(checkSbeRestrictedOrFullyEnabled(db) &&
          FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'SearchInSbe'))) {
        // Tests that getMore has a correct cursorOptions field.
        getMoreCase(coll, collUUID, standaloneConn, stConn);

        testSearchWithinLookup(db, coll, standaloneConn, stConn);
    }

    // Test that the docsRequested field makes it to the shards in a sharded environment when
    // $$SEARCH_META is referenced in the query.
    if (stConn != null) {
        searchMetaAfterLimit(coll, collUUID, stConn);
    }
}

function setupAndRunTestStandalone() {
    const mongotmock = new MongotMock();
    mongotmock.start();
    const mongotConn = mongotmock.getConnection();
    const conn = MongoRunner.runMongod({setParameter: {mongotHost: mongotConn.host}});
    let db = conn.getDB(dbName);
    if (FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'SearchBatchSizeTuning')) {
        jsTestLog("Skipping the test because it only applies when batchSize isn't enabled.");
    } else {
        let coll = db.getCollection(collName);
        // Insert documents.
        assert.commandWorked(coll.insertMany(docs));

        let collUUID = getUUIDFromListCollections(db, collName);
        runTest(db, collUUID, mongotConn, null /* stConn */);
    }

    MongoRunner.stopMongod(conn);
    mongotmock.stop();
}

function setupAndRunTestShardedEnv() {
    const stWithMock = new ShardingTestWithMongotMock({
        name: "search_docsrequested",
        shards: {
            rs0: {nodes: 2},
            rs1: {nodes: 2},
        },
        mongos: 1,
        other: {
            rsOptions: {setParameter: {enableTestCommands: 1}},
        }
    });
    stWithMock.start();
    let st = stWithMock.st;
    let mongos = st.s;
    let db = mongos.getDB(dbName);
    if (FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'SearchBatchSizeTuning')) {
        jsTestLog("Skipping the test because it only applies when batchSize isn't enabled.");
    } else {
        assert.commandWorked(mongos.getDB("admin").runCommand(
            {enableSharding: dbName, primaryShard: st.shard0.name}));

        let coll = db.getCollection(collName);

        // Insert documents.
        assert.commandWorked(coll.insertMany(docs));

        // Shard the collection, split it at {_id: chunkBoundary}, and move the higher chunk to
        // shard1.
        st.shardColl(coll, {_id: 1}, {_id: chunkBoundary}, {_id: chunkBoundary + 1});

        let collUUID = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);
        runTest(db, collUUID, null /* standaloneConn */, stWithMock);
    }

    stWithMock.stop();
}

// Test standalone.
setupAndRunTestStandalone();

// Test sharded cluster.
setupAndRunTestShardedEnv();
