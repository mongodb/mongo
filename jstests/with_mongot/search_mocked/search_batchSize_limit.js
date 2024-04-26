/**
 * Tests that the batchSize field is sent to mongot correctly when the query has an extractable
 * limit and the entire query is satisfied in the first mongot batch.
 * @tags: [featureFlagSearchBatchSizeTuning]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/sbe_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForQuery,
    MongotMock,
    mongotResponseForBatch
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

const dbName = "test";
const collName = jsTestName();

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

const docs = [
    {"_id": 1, "title": "Theme from New York, New York", "artist": "Frank Sinatra"},
    {"_id": 2, "title": "VeggieTales Theme Song", "artist": "Kidz Bop"},
    {"_id": 3, "title": "Hedwig's Theme", "artist": "John Williams"},
    {"_id": 4, "title": "Indiana Jones Main Theme", "artist": "John Williams"},
    {"_id": 5, "title": "Star Wars (Main Theme)", "artist": "John Williams"},
    {"_id": 6, "title": "SpongeBob SquarePants Theme Song", "artist": "Squidward"},
    {"_id": 7, "title": "Call Me, Beep Me! (The Kim Possible Theme)", "artist": "Kim"},
    {
        "_id": 8,
        "title": "My Heart Will Go On - Love Theme from \"Titanic\"",
        "artist": "Celine Dion"
    },
    {"_id": 9, "title": "Rocky Theme", "artist": "Bill Conti"},
    {"_id": 10, "title": "Mia and Sebastian's Theme", "artist": "Justin Hurwitz"},
    {"_id": 11, "title": "Barney's Theme Song", "artist": "Barney"},
    {"_id": 12, "title": "So Long, London", "artist": "Taylor Swift"},
    {"_id": 13, "title": "The Office Theme (from The Office)", "artist": "Jim and Pam"},
    {"_id": 14, "title": "Theme from Jurassic Park", "artist": "John Williams"},
    {"_id": 15, "title": "Theme from Superman - Concert Version", "artist": "John Williams"},
    {"_id": 16, "title": "Ghostbusters - Instrumental Version", "artist": "Ray Parker Jr."},
    {"_id": 17, "title": "Full House Theme", "artist": "Jesse Frederick"},
    {"_id": 18, "title": "The James Bond Theme", "artist": "Monty Normon"},
    {
        "_id": 19,
        "title": "Twelve Variations on Vous dirai-je, Mama",
        "artist": "Wolfgang Amadeus Mozart"
    },
    {"_id": 20, "title": "Theme (from \"Spider Man\")", "artist": "Francis Webster and Bob Harris"},
];
assert.commandWorked(coll.insertMany(docs));

const collUUID = getUUIDFromListCollections(db, coll.getName());
let mongotQuery = {query: "Theme", path: "title"};

// All the documents that would be returned by the search query above.
let relevantDocs = [];
let relevantSearchDocs = [];
let relevantStoredSourceDocs = [];
let relevantDocsOnlyTitle = [];
let searchScore = 0.300;
for (let i = 0; i < docs.length; i++) {
    if (docs[i]["title"].includes(mongotQuery.query)) {
        relevantDocs.push(docs[i]);
        relevantSearchDocs.push({_id: docs[i]._id, $searchScore: searchScore});
        relevantStoredSourceDocs.push({storedSource: docs[i], $searchScore: searchScore});
        relevantDocsOnlyTitle.push({title: docs[i].title})
    }

    // The documents with lower _id will have a higher search score.
    searchScore = searchScore - 0.001;
}

assert.eq(17, relevantDocs.length);

function runTest(oversubscriptionFactor, mongotQuery) {
    // When we have a computed batchSize less than 10, it should be rounded to minimum 10 when
    // computing initial mongot batchSize.
    function calcNumDocsMongotShouldReturn(extractedLimit) {
        return Math.max(Math.ceil(oversubscriptionFactor * extractedLimit), 10);
    }

    function mockRequest(extractedLimit) {
        const cursorId = NumberLong(99);
        const responseOk = 1;
        const batchSize = calcNumDocsMongotShouldReturn(extractedLimit);

        const docsToReturn = mongotQuery.returnStoredSource
            ? relevantStoredSourceDocs.slice(0, batchSize)
            : relevantSearchDocs.slice(0, batchSize);
        const history = [{
            expectedCommand: mongotCommandForQuery(mongotQuery,
                                                   collName,
                                                   dbName,
                                                   collUUID,
                                                   /*protocolVersion*/ null,
                                                   /*cursorOptions*/ {batchSize}),
            response:
                mongotResponseForBatch(docsToReturn, NumberLong(0), coll.getFullName(), responseOk),
        }];
        mongotMock.setMockResponses(history, cursorId);
    }

    /**
     * Mocks the mongot request / results for the given pipeline with the given extractedLimit,
     * then asserts that the results are correct.
     * This will fail (via the mongotMock internals) if the batchSize sent to mongot is different
     * than expected.
     */
    function runBatchSizeExtractableLimitTest({pipeline, extractedLimit, expectedDocs}) {
        mockRequest(extractedLimit);
        let res = coll.aggregate(pipeline).toArray();
        assert.eq(expectedDocs, res);
    }

    runBatchSizeExtractableLimitTest({
        pipeline: [{$search: mongotQuery}, {$limit: 13}],
        extractedLimit: 13,
        expectedDocs: relevantDocs.slice(0, 13)
    });

    runBatchSizeExtractableLimitTest({
        pipeline: [{$search: mongotQuery}, {$limit: 17}],
        extractedLimit: 17,
        expectedDocs: relevantDocs
    });

    runBatchSizeExtractableLimitTest({
        pipeline: [{$search: mongotQuery}, {$limit: 20}],
        extractedLimit: 20,
        expectedDocs: relevantDocs
    });

    runBatchSizeExtractableLimitTest({
        pipeline: [{$search: mongotQuery}, {$limit: 500}],
        extractedLimit: 500,
        expectedDocs: relevantDocs
    });

    runBatchSizeExtractableLimitTest({
        pipeline: [{$search: mongotQuery}, {$limit: 500}, {$limit: 13}],
        extractedLimit: 13,
        expectedDocs: relevantDocs.slice(0, 13)
    });

    runBatchSizeExtractableLimitTest({
        pipeline: [{$search: mongotQuery}, {$project: {_id: 0, artist: 0}}, {$limit: 13}],
        extractedLimit: 13,
        expectedDocs: relevantDocsOnlyTitle.slice(0, 13)
    });

    runBatchSizeExtractableLimitTest({
        pipeline: [{$search: mongotQuery}, {$limit: 5}],
        extractedLimit: 5,
        expectedDocs: relevantDocs.slice(0, 5)
    });

    runBatchSizeExtractableLimitTest({
        pipeline: [{$search: mongotQuery}, {$limit: 13}, {$limit: 5}],
        extractedLimit: 5,
        expectedDocs: relevantDocs.slice(0, 5)
    });

    runBatchSizeExtractableLimitTest({
        pipeline: [
            {$search: mongotQuery},
            {$limit: 7},
            {$limit: 500},
            {$project: {_id: 0, title: 1}},
            {$limit: 13}
        ],
        extractedLimit: 7,
        expectedDocs: relevantDocsOnlyTitle.slice(0, 7)
    });

    runBatchSizeExtractableLimitTest({
        pipeline: [{$search: mongotQuery}, {$skip: 5}, {$limit: 10}],
        extractedLimit: 15,
        expectedDocs: relevantDocs.slice(5, 15)
    });

    runBatchSizeExtractableLimitTest({
        pipeline:
            [{$search: mongotQuery}, {$skip: 5}, {$limit: 3}, {$project: {_id: 0, artist: 0}}],
        extractedLimit: 8,
        expectedDocs: relevantDocsOnlyTitle.slice(5, 8)
    });

    runBatchSizeExtractableLimitTest({
        pipeline: [{$search: mongotQuery}, {$skip: 15}, {$limit: 100}],
        extractedLimit: 115,
        expectedDocs: relevantDocs.slice(15)
    });

    runBatchSizeExtractableLimitTest({
        pipeline: [{$search: mongotQuery}, {$skip: 5}, {$limit: 10}, {$skip: 5}, {$limit: 3}],
        extractedLimit: 13,
        expectedDocs: relevantDocs.slice(10, 13)
    });
}

let oversubscriptionFactor = 1.064;
// Assert the default oversubscriptionFactor is 1.064.
assert.eq(oversubscriptionFactor,
          assert.commandWorked(db.adminCommand({getClusterParameter: "internalSearchOptions"}))
              .clusterParameters[0]
              .oversubscriptionFactor);
runTest(oversubscriptionFactor, mongotQuery);

oversubscriptionFactor = 1;
assert.commandWorked(
    db.adminCommand({setClusterParameter: {internalSearchOptions: {oversubscriptionFactor}}}));
runTest(oversubscriptionFactor, mongotQuery);

oversubscriptionFactor = 1.5;
assert.commandWorked(
    db.adminCommand({setClusterParameter: {internalSearchOptions: {oversubscriptionFactor}}}));
runTest(oversubscriptionFactor, mongotQuery);

mongotQuery.returnStoredSource = true;
// Although the actual cluster parameter oversubscriptionFactor is still 1.5, the oversubscription
// should not be applied for stored source queries.
oversubscriptionFactor = 1;
runTest(oversubscriptionFactor, mongotQuery);

MongoRunner.stopMongod(conn);
mongotMock.stop();
