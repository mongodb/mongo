/**
 * Tests that the batchSize field is sent to mongot correctly on the initial request.
 * @tags: [requires_fcv_81]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/sbe_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    getDefaultProtocolVersionForPlanShardedSearch,
    mockPlanShardedSearchResponse,
    mongotCommandForQuery,
    MongotMock,
    mongotMultiCursorResponseForBatch,
    mongotResponseForBatch
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = "test";
const collName = jsTestName();

const docs = [
    {"_id": 1, "title": "Theme from New York, New York", "artist": "Frank Sinatra", streams: 1424},
    {"_id": 2, "title": "VeggieTales Theme Song", "artist": "Kidz Bop", streams: 24},
    {"_id": 3, "title": "Hedwig's Theme", "artist": "John Williams", streams: 947},
    {"_id": 4, "title": "Indiana Jones Main Theme", "artist": "John Williams", streams: 1191},
    {"_id": 5, "title": "Star Wars (Main Theme)", "artist": "John Williams", streams: 2024},
    {"_id": 6, "title": "SpongeBob SquarePants Theme Song", "artist": "Squidward", streams: 742},
    {
        "_id": 7,
        "title": "Call Me, Beep Me! (The Kim Possible Theme)",
        "artist": "Kim",
        streams: 598
    },
    {
        "_id": 8,
        "title": "My Heart Will Go On - Love Theme from \"Titanic\"",
        "artist": "Celine Dion",
        streams: 2522
    },
    {"_id": 9, "title": "Rocky Theme", "artist": "Bill Conti", streams: 1329},
    {"_id": 10, "title": "Mia and Sebastian's Theme", "artist": "Justin Hurwitz", streams: 5939},
    {"_id": 11, "title": "Barney's Theme Song", "artist": "Barney", streams: 99},
    {"_id": 12, "title": "So Long, London", "artist": "Taylor Swift", streams: 1989},
    {
        "_id": 13,
        "title": "The Office Theme (from The Office)",
        "artist": "Jim and Pam",
        streams: 876
    },
    {"_id": 14, "title": "Theme from Jurassic Park", "artist": "John Williams", streams: 1502},
    {
        "_id": 15,
        "title": "Theme from Superman - Concert Version",
        "artist": "John Williams",
        streams: 901
    },
    {
        "_id": 16,
        "title": "Ghostbusters - Instrumental Version",
        "artist": "Ray Parker Jr.",
        streams: 1049
    },
    {"_id": 17, "title": "Full House Theme", "artist": "Jesse Frederick", streams: 692},
    {"_id": 18, "title": "The James Bond Theme", "artist": "Monty Normon", streams: 1320},
    {
        "_id": 19,
        "title": "Twelve Variations on Vous dirai-je, Mama",
        "artist": "Wolfgang Amadeus Mozart",
        streams: 390
    },
    {
        "_id": 20,
        "title": "Theme (from \"Spider Man\")",
        "artist": "Francis Webster and Bob Harris",
        streams: 182
    },
];

let mongotQuery = {query: "Theme", path: "title"};

// All the documents that would be returned by the search query above.
let relevantDocs = [];
let relevantDocsSortedByStreams = [];
let relevantSearchDocs = [];
let relevantStoredSourceDocs = [];
let relevantDocsOnlyTitle = [];
let searchScore = 0.300;
for (let i = 0; i < docs.length; i++) {
    if (docs[i][mongotQuery.path].includes(mongotQuery.query)) {
        relevantDocs.push(docs[i]);
        relevantDocsSortedByStreams.push(docs[i]);
        relevantSearchDocs.push({_id: docs[i]._id, $searchScore: searchScore});
        relevantStoredSourceDocs.push({storedSource: docs[i], $searchScore: searchScore});
        relevantDocsOnlyTitle.push({title: docs[i].title});
    }

    // The documents with lower _id will have a higher search score.
    searchScore = searchScore - 0.001;
}

relevantDocsSortedByStreams.sort((a, b) => {
    return b.streams - a.streams;
});
assert.eq(17, relevantDocs.length);

const kDefaultMongotBatchSize = 101;
const kDefaultOversubscriptionFactor = 1.064;

/**
 * Runs all test cases on the provided db connection. mockRequestFn is a callback to mock the mongot
 * responses since the mocking logic is different for standalone and sharded cases.
 */
function runTest(db, mockRequestFn) {
    /**
     * The oversubscriptionFactor is customizable as a cluster parameter. This helper function
     * asserts the value is set as expected.
     */
    function assertOversubscriptionSetAsExpected(expectedOversubscription) {
        assert.eq(
            expectedOversubscription,
            assert.commandWorked(db.adminCommand({getClusterParameter: "internalSearchOptions"}))
                .clusterParameters[0]
                .oversubscriptionFactor);
    }

    /**
     * Mocks the mongot request / results for the given pipeline with the computed batchSize,
     * then asserts that the results are correct. This will fail (via the mongotMock internals) if
     * the batchSize sent to mongot is different than expected.
     */
    function runInitialBatchSizeTest(
        {pipeline, expectedDocs, expectedBatchSize, isStoredSource = false}) {
        mockRequestFn(expectedBatchSize, isStoredSource);
        let res = coll.aggregate(pipeline).toArray();
        assert.eq(expectedDocs, res);
    }

    // The initial tests should run with returnStoredSource=false. We'll enable it in the final
    // cases.
    mongotQuery.returnStoredSource = false;
    let coll = db.getCollection(collName);

    // First, test a variety of cases with the default oversubscription rate.
    {
        // Runs a pipeline with extractable limit greater than default batchSize.
        runInitialBatchSizeTest({
            pipeline: [{$search: mongotQuery}, {$limit: 500}],
            expectedBatchSize: Math.ceil(500 * kDefaultOversubscriptionFactor),
            expectedDocs: relevantDocs,
        });

        // Runs a pipeline with extractable limit less than default batchSize.
        runInitialBatchSizeTest({
            pipeline: [{$search: mongotQuery}, {$limit: 500}, {$skip: 15}, {$limit: 15}],
            expectedBatchSize: Math.ceil(30 * kDefaultOversubscriptionFactor),
            expectedDocs: relevantDocs.slice(15),
        });

        // Runs a pipeline with an extractable limit less than 10; we still request at least 10.
        runInitialBatchSizeTest({
            pipeline:
                [{$search: mongotQuery}, {$project: {_id: 0, artist: 0, streams: 0}}, {$limit: 5}],
            expectedBatchSize: 10,
            expectedDocs: relevantDocsOnlyTitle.slice(0, 5),
        });

        // Runs a pipeline that requires all documents due to blocking stage $sort.
        runInitialBatchSizeTest({
            pipeline: [{$search: mongotQuery}, {$sort: {streams: -1}}],
            expectedBatchSize: Math.ceil(kDefaultMongotBatchSize * kDefaultOversubscriptionFactor),
            expectedDocs: relevantDocsSortedByStreams,
        });

        // Runs a pipeline that applies a limit before a blocking stage $sort.
        runInitialBatchSizeTest({
            pipeline: [{$search: mongotQuery}, {$limit: 500}, {$sort: {streams: -1}}, {$limit: 5}],
            expectedBatchSize: Math.ceil(500 * kDefaultOversubscriptionFactor),
            expectedDocs: relevantDocsSortedByStreams.slice(0, 5),
        });

        // Runs a pipeline that applies a filter before a blocking stage $sort.
        runInitialBatchSizeTest({
            pipeline:
                [{$search: mongotQuery}, {$match: {streams: {$gt: 1500}}}, {$sort: {streams: -1}}],
            expectedBatchSize: Math.ceil(kDefaultMongotBatchSize * kDefaultOversubscriptionFactor),
            // There are 4 relevant documents with > 1500 streams.
            expectedDocs: relevantDocsSortedByStreams.slice(0, 4),
        });

        // Runs a pipeline with unknown docs needed.
        runInitialBatchSizeTest({
            pipeline: [{$search: mongotQuery}, {$match: {streams: {$gt: 1500}}}],
            expectedBatchSize: Math.ceil(kDefaultMongotBatchSize * kDefaultOversubscriptionFactor),
            expectedDocs: relevantDocs.filter((doc) => doc.streams > 1500),
        });

        // Runs a pipeline with a non-extractable limit (due to the $match before $limit).
        runInitialBatchSizeTest({
            pipeline: [
                {$search: mongotQuery},
                {$match: {streams: {$gt: 1500}}},
                {$limit: 98},
                {$sort: {streams: -1}}
            ],
            expectedBatchSize: Math.ceil(98 * kDefaultOversubscriptionFactor),
            expectedDocs: relevantDocsSortedByStreams.slice(0, 4),
        });

        // Runs a pipeline with a non-extractable limit, where the computed batchSize is less than
        // the default batchSize; in that case, we round up to default batchSize.
        runInitialBatchSizeTest({
            pipeline: [
                {$search: mongotQuery},
                {$match: {streams: {$gt: 1500}}},
                {$limit: 50},
                {$sort: {streams: -1}}
            ],
            expectedBatchSize: kDefaultMongotBatchSize,
            expectedDocs: relevantDocsSortedByStreams.slice(0, 4),
        });

        // $unwind with preserveNullAndEmptyArrays: true before a $limit is a special case since it
        // produces DocNeededBounds where the minimum bounds are unknown and the maximum bounds are
        // a discrete value set by the $limit value. The initial batchSize should be computed as the
        // $limit value with oversubscription applied, clipped to the range [10, 101].
        runInitialBatchSizeTest({
            pipeline: [
                {$search: mongotQuery},
                {$unwind: {path: "$nonexistentArrayField", preserveNullAndEmptyArrays: true}},
                {$project: {_id: 0, title: 1}},
                {$limit: 5}
            ],
            expectedBatchSize: 10,
            expectedDocs: relevantDocsOnlyTitle.slice(0, 5),
        });

        runInitialBatchSizeTest({
            pipeline: [
                {$search: mongotQuery},
                {$unwind: {path: "$nonexistentArrayField", preserveNullAndEmptyArrays: true}},
                {$project: {_id: 0, title: 1}},
                {$limit: 500}
            ],
            expectedBatchSize: kDefaultMongotBatchSize,
            expectedDocs: relevantDocsOnlyTitle,
        });

        runInitialBatchSizeTest({
            pipeline: [
                {$search: mongotQuery},
                {$unwind: {path: "$nonexistentArrayField", preserveNullAndEmptyArrays: true}},
                {$project: {_id: 0, title: 1}},
                {$limit: 60}
            ],
            expectedBatchSize: Math.ceil(60 * kDefaultOversubscriptionFactor),
            expectedDocs: relevantDocsOnlyTitle,
        });
    }

    // Now we'll assert that the oversubscriptionFactor is properly configurable and that the new
    // values are applied correctly to some basic pipelines.
    {
        assertOversubscriptionSetAsExpected(kDefaultOversubscriptionFactor);

        // Run pipelines with overSubscriptionFactor set to 1.
        let oversubscriptionFactor = 1;
        assert.commandWorked(db.adminCommand(
            {setClusterParameter: {internalSearchOptions: {oversubscriptionFactor}}}));
        assertOversubscriptionSetAsExpected(oversubscriptionFactor);
        runInitialBatchSizeTest({
            pipeline: [{$search: mongotQuery}, {$project: {_id: 0, title: 1}}],
            expectedBatchSize: kDefaultMongotBatchSize,
            expectedDocs: relevantDocsOnlyTitle,
        });
        runInitialBatchSizeTest({
            pipeline: [{$search: mongotQuery}, {$limit: 40}],
            expectedBatchSize: 40,
            expectedDocs: relevantDocs,
        });

        // Run pipelines with overSubscriptionFactor set to 1.8.
        oversubscriptionFactor = 1.8;
        assert.commandWorked(db.adminCommand(
            {setClusterParameter: {internalSearchOptions: {oversubscriptionFactor}}}));
        assertOversubscriptionSetAsExpected(oversubscriptionFactor);
        runInitialBatchSizeTest({
            pipeline: [{$search: mongotQuery}, {$project: {_id: 0, title: 1}}],
            expectedBatchSize: Math.ceil(kDefaultMongotBatchSize * oversubscriptionFactor),
            expectedDocs: relevantDocsOnlyTitle,
        });
        runInitialBatchSizeTest({
            pipeline: [{$search: mongotQuery}, {$limit: 40}],
            expectedBatchSize: Math.ceil(40 * oversubscriptionFactor),
            expectedDocs: relevantDocs,
        });

        // Run pipelines using the returnStoredSource option. Even though the oversubscriptionFactor
        // is still set to 1.8, it should not be applied for stored source queries.
        mongotQuery.returnStoredSource = true;
        runInitialBatchSizeTest({
            pipeline: [{$search: mongotQuery}, {$project: {_id: 0, title: 1}}],
            expectedBatchSize: kDefaultMongotBatchSize,
            expectedDocs: relevantDocsOnlyTitle,
            isStoredSource: true
        });
        runInitialBatchSizeTest({
            pipeline: [{$search: mongotQuery}, {$limit: 40}],
            expectedBatchSize: 40,
            expectedDocs: relevantDocs,
            isStoredSource: true
        });
    }
}

// Run the tests for a standalone mongod.
{
    // Start mock mongot.
    const mongotMock = new MongotMock();
    mongotMock.start();
    const mockConn = mongotMock.getConnection();

    // Start mongod.
    const conn = MongoRunner.runMongod({setParameter: {mongotHost: mockConn.host}});
    let db = conn.getDB(dbName);
    let coll = db.getCollection(collName);
    coll.drop();

    if (checkSbeRestrictedOrFullyEnabled(db) &&
        FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'SearchInSbe')) {
        jsTestLog("Skipping the test because it only applies to $search in classic engine.");
        MongoRunner.stopMongod(conn);
        mongotMock.stop();
        quit();
    }

    assert.commandWorked(coll.insertMany(docs));
    const collUUID = getUUIDFromListCollections(db, coll.getName());

    const mockRequestFn = (batchSize, isStoredSource) => {
        const cursorId = NumberLong(99);
        const responseOk = 1;

        const docsToReturn = isStoredSource ? relevantStoredSourceDocs.slice(0, batchSize)
                                            : relevantSearchDocs.slice(0, batchSize);
        const history = [{
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: collName,
                db: dbName,
                collectionUUID: collUUID,
                cursorOptions: {batchSize}
            }),
            response:
                mongotResponseForBatch(docsToReturn, NumberLong(0), coll.getFullName(), responseOk),
        }];
        mongotMock.setMockResponses(history, cursorId);
    };

    runTest(db, mockRequestFn);

    MongoRunner.stopMongod(conn);
    mongotMock.stop();
}

// Run the tests for a sharded cluster.
{
    const chunkBoundary = 10;
    const protocolVersion = getDefaultProtocolVersionForPlanShardedSearch();

    const stWithMock = new ShardingTestWithMongotMock({
        name: jsTestName(),
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
    let coll = db.getCollection(collName);
    coll.drop();

    assert.commandWorked(
        mongos.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

    assert.commandWorked(coll.insertMany(docs));
    // Shard the collection, split it at {_id: chunkBoundary}, and move the higher chunk to
    // shard1.
    st.shardColl(coll, {_id: 1}, {_id: chunkBoundary}, {_id: chunkBoundary + 1});
    const relevantDocsShard0 = relevantSearchDocs.filter((doc) => doc._id < chunkBoundary);
    const relevantDocsShard1 = relevantSearchDocs.filter((doc) => doc._id >= chunkBoundary);
    const relevantStoredSourceDocsShard0 =
        relevantStoredSourceDocs.filter((doc) => doc.storedSource._id < chunkBoundary);
    const relevantStoredSourceDocsShard1 =
        relevantStoredSourceDocs.filter((doc) => doc.storedSource._id >= chunkBoundary);

    const collUUID = getUUIDFromListCollections(db, coll.getName());

    const mockRequestFn = (batchSize, isStoredSource) => {
        const cursorId = NumberLong(123);
        const metaId = NumberLong(2);
        const responseOk = 1;

        const shard0DocsToReturn = isStoredSource
            ? relevantStoredSourceDocsShard0.slice(0, batchSize)
            : relevantDocsShard0.slice(0, batchSize);
        const shard1DocsToReturn = isStoredSource
            ? relevantStoredSourceDocsShard1.slice(0, batchSize)
            : relevantDocsShard1.slice(0, batchSize);
        const historyShard0 = [{
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: collName,
                db: dbName,
                collectionUUID: collUUID,
                protocolVersion: protocolVersion,
                cursorOptions: {batchSize}
            }),
            response: mongotMultiCursorResponseForBatch(shard0DocsToReturn,
                                                        NumberLong(0),
                                                        [{metaVal: 1}],
                                                        NumberLong(0),
                                                        coll.getFullName(),
                                                        responseOk),
        }];
        const s0Mongot = stWithMock.getMockConnectedToHost(stWithMock.st.rs0.getPrimary());
        s0Mongot.setMockResponses(historyShard0, cursorId, metaId);

        const historyShard1 = [{
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: collName,
                db: dbName,
                collectionUUID: collUUID,
                protocolVersion: protocolVersion,
                cursorOptions: {batchSize}
            }),
            response: mongotMultiCursorResponseForBatch(shard1DocsToReturn,
                                                        NumberLong(0),
                                                        [{metaVal: 1}],
                                                        NumberLong(0),
                                                        coll.getFullName(),
                                                        responseOk),
        }];
        const s1Mongot = stWithMock.getMockConnectedToHost(stWithMock.st.rs1.getPrimary());
        s1Mongot.setMockResponses(historyShard1, cursorId, metaId);

        mockPlanShardedSearchResponse(
            collName, mongotQuery, dbName, undefined /*sortSpec*/, stWithMock);
    };

    runTest(db, mockRequestFn);

    stWithMock.stop();
}
