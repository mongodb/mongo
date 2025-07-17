/**
 * Tests that "searchRootDocumentId" metadata is properly plumbed through the $search agg stage
 * in both unsharded and sharded scenarios.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mockPlanShardedSearchResponse,
    mongotCommandForQuery,
    MongotMock,
    mongotMultiCursorResponseForBatch,
    mongotResponseForBatch,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = "test";
const docOneSearchRootDocId = ObjectId("507f1f77bcf86cd799439011");
const docTwoSearchRootDocId = ObjectId("507f1f77bcf86cd799439012");
const docThreeSearchRootDocId = ObjectId("507f1f77bcf86cd799439013");
const docFourSearchRootDocId = ObjectId("507f1f77bcf86cd799439014");
const docFiveSearchRootDocId = ObjectId("507f1f77bcf86cd799439015");
const shardingBoundary = ObjectId("507f1f77bcf86cd799439030");
const shardedDocOneId = ObjectId("507f1f77bcf86cd799439021");
const shardedDocTwoId = ObjectId("507f1f77bcf86cd799439022");
const shardedDocThreeId = ObjectId("507f1f77bcf86cd799439023");

function runUnshardedTests() {
    const mongotMock = new MongotMock();
    mongotMock.start();
    const mockConn = mongotMock.getConnection();

    const conn = MongoRunner.runMongod({
        setParameter: {mongotHost: mockConn.host},
    });

    const testDB = conn.getDB(dbName);
    const coll = testDB.search_root_document_id;

    // Test data with nested structure for directors and actors.
    assert.commandWorked(coll.insert({
        _id: docOneSearchRootDocId,
        movieInfo: {
            title: "The Shawshank Redemption",
            genre: "Drama",
            director: "Frank Darabont",
            actors: ["Tim Robbins", "Morgan Freeman"]
        }
    }));
    assert.commandWorked(coll.insert({
        _id: docTwoSearchRootDocId,
        movieInfo: {
            title: "The Godfather",
            genre: "Crime",
            director: "Francis Ford Coppola",
            actors: ["Marlon Brando", "Al Pacino"]
        }
    }));
    assert.commandWorked(coll.insert({
        _id: docThreeSearchRootDocId,
        movieInfo: {
            title: "The Dark Knight",
            genre: "Action",
            director: {first: "Christopher", last: "Nolan"},
            actors: ["Christian Bale", "Heath Ledger"]
        }
    }));
    assert.commandWorked(coll.insert({
        _id: docFiveSearchRootDocId,
        movieInfo: {
            title: "Pulp Fiction",
            genre: "Crime",
            director: {first: "Quentin", last: "Tarantino"},
            actors: ["John Travolta", "Uma Thurman", "Samuel L. Jackson"]
        }
    }));
    assert.commandWorked(coll.insert({
        _id: docFourSearchRootDocId,
        director: "M. Night Shyamalan",
        birthDate: "1970-08-06",
        movieInfo: [
            {
                title: "The Sixth Sense",
                releaseYear: 1999,
                genre: "Thriller",
                reviews: [
                    {rating: 9.0, text: "Mind-blowing and suspenseful"},
                    {rating: 8.5, text: "Incredible plot twist"},
                    {rating: 5.5, text: "Amazing plot twist"}
                ]
            },
            {
                title: "Split",
                releaseYear: 2016,
                genre: "Thriller",
                reviews: [
                    {rating: 8.0, text: "Intense psychological thriller"},
                    {rating: 7.5, text: "Amazing lead performance"}
                ]
            }
        ]
    }));
    const collUUID = getUUIDFromListCollections(testDB, coll.getName());

    // Test basic searchRootDocumentId functionality.
    {
        const mongotQuery = {returnStoredSource: true, returnScope: {path: "movieInfo"}};
        const cursorId = NumberLong(123);
        const pipeline = [
            {$search: mongotQuery},
            {$addFields: {searchRootDocumentId: {$meta: "searchRootDocumentId"}}}
        ];

        const mongotResponseBatch = [
            {
                storedSource: {
                    title: "The Shawshank Redemption",
                    genre: "Drama",
                    director: "Frank Darabont",
                    actors: ["Tim Robbins", "Morgan Freeman"]
                },
                $searchRootDocumentId: docOneSearchRootDocId
            },
            {
                storedSource: {
                    title: "The Godfather",
                    genre: "Crime",
                    director: "Francis Ford Coppola",
                    actors: ["Marlon Brando", "Al Pacino"]
                },
                $searchRootDocumentId: docTwoSearchRootDocId
            }
        ];

        const expectedDocs = [
            {
                title: "The Shawshank Redemption",
                genre: "Drama",
                director: "Frank Darabont",
                actors: ["Tim Robbins", "Morgan Freeman"],
                searchRootDocumentId: docOneSearchRootDocId
            },
            {
                title: "The Godfather",
                genre: "Crime",
                director: "Francis Ford Coppola",
                actors: ["Marlon Brando", "Al Pacino"],
                searchRootDocumentId: docTwoSearchRootDocId
            }
        ];

        const history = [{
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: coll.getName(),
                db: dbName,
                collectionUUID: collUUID
            }),
            response:
                mongotResponseForBatch(mongotResponseBatch, NumberLong(0), coll.getFullName(), 1),
        }];

        mongotMock.setMockResponses(history, cursorId);
        const result = coll.aggregate(pipeline).toArray();
        assert.eq(result, expectedDocs);
    }

    // Check that metadata is passed along correctly when there are multiple batches, both between
    // the shell and mongod, and between mongod and mongot.
    {
        const mongotQuery = {returnStoredSource: true, returnScope: {path: "movieInfo"}};
        const cursorId = NumberLong(456);
        const pipeline = [
            {$search: mongotQuery},
            {$addFields: {searchRootDocumentId: {$meta: "searchRootDocumentId"}}}
        ];

        const batchOne = [{
            storedSource:
                {title: "The Shawshank Redemption", genre: "Drama", director: "Frank Darabont"},
            $searchRootDocumentId: docOneSearchRootDocId
        }];

        const batchTwo = [{
            storedSource:
                {title: "The Godfather", genre: "Crime", director: "Francis Ford Coppola"},
            $searchRootDocumentId: docTwoSearchRootDocId
        }];

        const batchThree = [{
            storedSource: {
                title: "The Dark Knight",
                genre: "Action",
                director: {first: "Christopher", last: "Nolan"}
            },
            $searchRootDocumentId: docThreeSearchRootDocId
        }];

        const expectedDocs = [
            {
                title: "The Shawshank Redemption",
                genre: "Drama",
                director: "Frank Darabont",
                searchRootDocumentId: docOneSearchRootDocId
            },
            {
                title: "The Godfather",
                genre: "Crime",
                director: "Francis Ford Coppola",
                searchRootDocumentId: docTwoSearchRootDocId
            },
            {
                title: "The Dark Knight",
                genre: "Action",
                director: {first: "Christopher", last: "Nolan"},
                searchRootDocumentId: docThreeSearchRootDocId
            }
        ];

        const history = [
            {
                expectedCommand: mongotCommandForQuery({
                    query: mongotQuery,
                    collName: coll.getName(),
                    db: dbName,
                    collectionUUID: collUUID
                }),
                response: mongotResponseForBatch(batchOne, cursorId, coll.getFullName(), 1),
            },
            {
                expectedCommand: {getMore: cursorId, collection: coll.getName()},
                response: mongotResponseForBatch(batchTwo, cursorId, coll.getFullName(), 1),
            },
            {
                expectedCommand: {getMore: cursorId, collection: coll.getName()},
                response: mongotResponseForBatch(batchThree, NumberLong(0), coll.getFullName(), 1),
            }
        ];

        mongotMock.setMockResponses(history, cursorId);
        assert.eq(coll.aggregate(pipeline, {cursor: {batchSize: 1}}).toArray(), expectedDocs);
    }

    // Test array documents where multiple child documents share the same searchRootDocumentId.
    {
        const mongotQuery = {returnStoredSource: true, returnScope: {path: "movieInfo"}};
        const cursorId = NumberLong(555);
        const pipeline = [
            {$search: mongotQuery},
            {$addFields: {searchRootDocumentId: {$meta: "searchRootDocumentId"}}}
        ];

        const mongotResponseBatch = [
            {
                storedSource: {
                    title: "The Sixth Sense",
                    releaseYear: 1999,
                    genre: "Thriller",
                    reviews: [
                        {rating: 9.0, text: "Mind-blowing and suspenseful"},
                        {rating: 8.5, text: "Incredible plot twist"},
                        {rating: 5.5, text: "Amazing plot twist"}
                    ]
                },
                $searchRootDocumentId: docFourSearchRootDocId
            },
            {
                storedSource: {
                    title: "Split",
                    releaseYear: 2016,
                    genre: "Thriller",
                    reviews: [
                        {rating: 8.0, text: "Intense psychological thriller"},
                        {rating: 7.5, text: "Amazing lead performance"}
                    ]
                },
                $searchRootDocumentId: docFourSearchRootDocId
            }
        ];

        const expectedDocs = [
            {
                title: "The Sixth Sense",
                releaseYear: 1999,
                genre: "Thriller",
                reviews: [
                    {rating: 9.0, text: "Mind-blowing and suspenseful"},
                    {rating: 8.5, text: "Incredible plot twist"},
                    {rating: 5.5, text: "Amazing plot twist"}
                ],
                searchRootDocumentId: docFourSearchRootDocId
            },
            {
                title: "Split",
                releaseYear: 2016,
                genre: "Thriller",
                reviews: [
                    {rating: 8.0, text: "Intense psychological thriller"},
                    {rating: 7.5, text: "Amazing lead performance"}
                ],
                searchRootDocumentId: docFourSearchRootDocId
            }
        ];

        const history = [{
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: coll.getName(),
                db: dbName,
                collectionUUID: collUUID
            }),
            response:
                mongotResponseForBatch(mongotResponseBatch, NumberLong(0), coll.getFullName(), 1),
        }];

        mongotMock.setMockResponses(history, cursorId);
        const result = coll.aggregate(pipeline).toArray();
        assert.eq(result, expectedDocs);
    }

    // Check null and missing metadata is handled properly.
    {
        const mongotQuery = {returnStoredSource: true, returnScope: {path: "movieInfo"}};
        const pipeline = [
            {$search: mongotQuery},
            {$addFields: {searchRootDocumentId: {$meta: "searchRootDocumentId"}}}
        ];

        // Test null searchRootDocumentId should throw error 13111.
        {
            const cursorId = NumberLong(789);
            const responseWithNull =
                [{storedSource: {title: "Error Movie"}, $searchRootDocumentId: null}];

            const history = [{
                expectedCommand: mongotCommandForQuery({
                    query: mongotQuery,
                    collName: coll.getName(),
                    db: dbName,
                    collectionUUID: collUUID
                }),
                response:
                    mongotResponseForBatch(responseWithNull, NumberLong(0), coll.getFullName(), 1),
            }];

            mongotMock.setMockResponses(history, cursorId);
            assert.commandFailedWithCode(
                testDB.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}),
                13111);
        }

        // Test missing searchRootDocumentId field should succeed but not include the metadata.
        {
            const cursorId = NumberLong(790);
            const responseWithMissing = [{storedSource: {title: "Missing Movie"}}];

            const historyMissing = [{
                expectedCommand: mongotCommandForQuery({
                    query: mongotQuery,
                    collName: coll.getName(),
                    db: dbName,
                    collectionUUID: collUUID
                }),
                response: mongotResponseForBatch(
                    responseWithMissing, NumberLong(0), coll.getFullName(), 1),
            }];

            mongotMock.setMockResponses(historyMissing, cursorId);
            const resultMissing = coll.aggregate(pipeline).toArray();
            assert.eq(resultMissing, [{title: "Missing Movie"}]);
        }
    }

    {
        // Test error: returnScope is missing.
        const pipeline1 = [
            {$search: {returnStoredSource: true}},
            {$project: {searchRootDocumentId: {$meta: "searchRootDocumentId"}}}
        ];
        assert.commandFailedWithCode(
            testDB.runCommand({aggregate: coll.getName(), pipeline: pipeline1, cursor: {}}), 40218);

        // Test error: returnStoredSource is missing.
        const pipeline2 = [
            {$search: {returnScope: {path: "movieInfo"}}},
            {$project: {searchRootDocumentId: {$meta: "searchRootDocumentId"}}}
        ];
        assert.commandFailedWithCode(
            testDB.runCommand({aggregate: coll.getName(), pipeline: pipeline2, cursor: {}}), 40218);
    }

    // Dotted field path test.
    {
        const mongotQuery = {returnStoredSource: true, returnScope: {path: "movieInfo.director"}};
        const cursorId = NumberLong(999);
        const pipeline = [
            {$search: mongotQuery},
            {$addFields: {searchRootDocumentId: {$meta: "searchRootDocumentId"}}}
        ];

        const mongotResponseBatch = [
            {
                storedSource: {first: "Christopher", last: "Nolan"},
                $searchRootDocumentId: docThreeSearchRootDocId
            },
            {
                storedSource: {first: "Quentin", last: "Tarantino"},
                $searchRootDocumentId: docFiveSearchRootDocId
            }
        ];

        const expectedResults = [
            {first: "Christopher", last: "Nolan", searchRootDocumentId: docThreeSearchRootDocId},
            {first: "Quentin", last: "Tarantino", searchRootDocumentId: docFiveSearchRootDocId}
        ];

        const history = [{
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: coll.getName(),
                db: dbName,
                collectionUUID: collUUID
            }),
            response:
                mongotResponseForBatch(mongotResponseBatch, NumberLong(0), coll.getFullName(), 1),
        }];

        mongotMock.setMockResponses(history, cursorId);
        const result = coll.aggregate(pipeline).toArray();
        assert.eq(result, expectedResults);
    }

    mongotMock.stop();
    MongoRunner.stopMongod(conn);
}

runUnshardedTests();

function runShardedTests() {
    const collName = "internal_search_mongot_remote";

    const stWithMock = new ShardingTestWithMongotMock({
        name: "sharded_search_root_document_id",
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
    const st = stWithMock.st;

    const mongos = st.s;
    const testDB = mongos.getDB(dbName);
    assert.commandWorked(
        mongos.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

    const testColl = testDB.getCollection(collName);
    const collNS = testColl.getFullName();

    assert.commandWorked(testColl.insert({
        _id: shardedDocOneId,
        movieInfo: {
            title: "Inception",
            genre: "Sci-Fi",
            director: {first: "Christopher", last: "Nolan"},
            actors: ["Leonardo DiCaprio", "Marion Cotillard"]
        }
    }));
    assert.commandWorked(testColl.insert({
        _id: shardedDocTwoId,
        movieInfo: {
            title: "Casablanca",
            genre: "Romance",
            director: "Michael Curtiz",
            actors: ["Humphrey Bogart", "Ingrid Bergman"]
        }
    }));
    assert.commandWorked(testColl.insert({
        _id: shardedDocThreeId,
        movieInfo: {
            title: "Parasite",
            genre: "Thriller",
            director: {first: "Bong", last: "Joon-ho"},
            actors: ["Song Kang-ho", "Lee Sun-kyun"]
        }
    }));

    st.shardColl(testColl, {_id: 1}, {_id: shardingBoundary}, {_id: shardingBoundary});

    const collUUID0 = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);
    const collUUID1 = getUUIDFromListCollections(st.rs1.getPrimary().getDB(dbName), collName);

    const protocolVersion = NumberInt(1);
    const cursorId = NumberLong(123);
    const secondCursorId = NumberLong(cursorId + 1001);

    function runTestOnPrimaries(testFn) {
        testDB.getMongo().setReadPref("primary");
        testFn(st.rs0.getPrimary(), st.rs1.getPrimary());
    }

    function runTestOnSecondaries(testFn) {
        testDB.getMongo().setReadPref("secondary");
        testFn(st.rs0.getSecondary(), st.rs1.getSecondary());
    }

    // Basic searchRootDocumentId functionality in sharded scenario.
    function testBasicShardedCase(shard0Conn, shard1Conn) {
        const mongotQuery = {returnStoredSource: true, returnScope: {path: "movieInfo"}};
        const responseOk = 1;

        const mongot0ResponseBatch = [{
            storedSource: {
                title: "Inception",
                genre: "Sci-Fi",
                director: {first: "Christopher", last: "Nolan"},
                actors: ["Leonardo DiCaprio", "Marion Cotillard"]
            },
            $searchScore: 100,
            $searchRootDocumentId: shardedDocOneId
        }];

        const mongot1ResponseBatch = [{
            storedSource: {
                title: "Casablanca",
                genre: "Romance",
                director: "Michael Curtiz",
                actors: ["Humphrey Bogart", "Ingrid Bergman"]
            },
            $searchScore: 111,
            $searchRootDocumentId: shardedDocTwoId
        }];

        const history0 = [{
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: collName,
                db: dbName,
                collectionUUID: collUUID0,
                protocolVersion: protocolVersion
            }),
            response: mongotMultiCursorResponseForBatch(
                mongot0ResponseBatch, NumberLong(0), [{val: 1}], NumberLong(0), collNS, responseOk),
        }];

        const history1 = [{
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: collName,
                db: dbName,
                collectionUUID: collUUID1,
                protocolVersion: protocolVersion
            }),
            response: mongotMultiCursorResponseForBatch(
                mongot1ResponseBatch, NumberLong(0), [{val: 1}], NumberLong(0), collNS, responseOk),
        }];

        const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
        const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
        s0Mongot.setMockResponses(history0, cursorId, secondCursorId);
        s1Mongot.setMockResponses(history1, cursorId, secondCursorId);

        const pipeline = [
            {$search: mongotQuery},
            {$addFields: {searchRootDocumentId: {$meta: "searchRootDocumentId"}}}
        ];

        mockPlanShardedSearchResponse(
            testColl.getName(), mongotQuery, dbName, undefined, stWithMock);

        const result = testColl.aggregate(pipeline).toArray();
        assert.gt(result.length, 0, "Should return movie results");

        result.forEach(doc => {
            assert(doc.hasOwnProperty('title'), "Should have movie title");
            assert(doc.hasOwnProperty('genre'), "Should have movie genre");
            assert(doc.hasOwnProperty('director'), "Should have movie director");
            assert(doc.hasOwnProperty('searchRootDocumentId'), "Should have searchRootDocumentId");
        });
    }

    runTestOnPrimaries(testBasicShardedCase);
    runTestOnSecondaries(testBasicShardedCase);

    // Test error handling in sharded scenario.
    function testErrorShardedCase(shard0Conn, shard1Conn) {
        const mongotQuery = {returnStoredSource: true, returnScope: {path: "movieInfo"}};
        const responseOk = 1;

        const mongot0ResponseBatch = [
            {storedSource: {title: "Error Movie"}, $searchScore: 100, $searchRootDocumentId: null}
        ];

        const mongot1ResponseBatch = [{
            storedSource: {title: "Valid Movie"},
            $searchScore: 111,
            $searchRootDocumentId: shardedDocTwoId
        }];

        const history0 = [{
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: collName,
                db: dbName,
                collectionUUID: collUUID0,
                protocolVersion: protocolVersion
            }),
            response: mongotMultiCursorResponseForBatch(
                mongot0ResponseBatch, NumberLong(0), [{val: 1}], NumberLong(0), collNS, responseOk),
        }];

        const history1 = [{
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: collName,
                db: dbName,
                collectionUUID: collUUID1,
                protocolVersion: protocolVersion
            }),
            response: mongotMultiCursorResponseForBatch(
                mongot1ResponseBatch, NumberLong(0), [{val: 1}], NumberLong(0), collNS, responseOk),
        }];

        const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
        const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
        s0Mongot.setMockResponses(history0, cursorId, secondCursorId);
        s1Mongot.setMockResponses(history1, cursorId, secondCursorId);

        const pipeline = [
            {$search: mongotQuery},
            {$addFields: {searchRootDocumentId: {$meta: "searchRootDocumentId"}}}
        ];

        mockPlanShardedSearchResponse(
            testColl.getName(), mongotQuery, dbName, undefined, stWithMock);

        const err = assert.throws(() => testColl.aggregate(pipeline).toArray());
        assert.commandFailedWithCode(err, 13111);
    }

    runTestOnPrimaries(testErrorShardedCase);
    runTestOnSecondaries(testErrorShardedCase);

    // Test dotted path in sharded scenario.
    function testDottedPathShardedCase(shard0Conn, shard1Conn) {
        const mongotQuery = {returnStoredSource: true, returnScope: {path: "movieInfo.director"}};
        const responseOk = 1;

        const mongot0ResponseBatch = [{
            storedSource: {first: "Christopher", last: "Nolan"},
            $searchScore: 100,
            $searchRootDocumentId: shardedDocOneId
        }];

        const mongot1ResponseBatch = [{
            storedSource: {first: "Bong", last: "Joon-ho"},
            $searchScore: 111,
            $searchRootDocumentId: shardedDocThreeId
        }];

        const history0 = [{
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: collName,
                db: dbName,
                collectionUUID: collUUID0,
                protocolVersion: protocolVersion
            }),
            response: mongotMultiCursorResponseForBatch(
                mongot0ResponseBatch, NumberLong(0), [{val: 1}], NumberLong(0), collNS, responseOk),
        }];

        const history1 = [{
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: collName,
                db: dbName,
                collectionUUID: collUUID1,
                protocolVersion: protocolVersion
            }),
            response: mongotMultiCursorResponseForBatch(
                mongot1ResponseBatch, NumberLong(0), [{val: 1}], NumberLong(0), collNS, responseOk),
        }];

        const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
        const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
        s0Mongot.setMockResponses(history0, cursorId, secondCursorId);
        s1Mongot.setMockResponses(history1, cursorId, secondCursorId);

        const pipeline = [
            {$search: mongotQuery},
            {$addFields: {searchRootDocumentId: {$meta: "searchRootDocumentId"}}}
        ];

        mockPlanShardedSearchResponse(
            testColl.getName(), mongotQuery, dbName, undefined, stWithMock);

        const result = testColl.aggregate(pipeline).toArray();
        assert.gt(result.length, 0, "Should return director results");

        result.forEach(doc => {
            assert(doc.hasOwnProperty('first'), "Should have first field");
            assert(doc.hasOwnProperty('last'), "Should have last field");
            assert(doc.hasOwnProperty('searchRootDocumentId'), "Should have searchRootDocumentId");
        });
    }

    runTestOnPrimaries(testDottedPathShardedCase);
    runTestOnSecondaries(testDottedPathShardedCase);

    stWithMock.stop();
}

runShardedTests();
