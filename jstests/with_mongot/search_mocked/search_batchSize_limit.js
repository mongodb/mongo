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
    {"_id": 1, "title": "Theme from New York, New York"},
    {"_id": 2, "title": "VeggieTales Theme Song"},
    {"_id": 3, "title": "Hedwig's Theme"},
    {"_id": 4, "title": "Indiana Jones Main Theme"},
    {"_id": 5, "title": "Star Wars (Main Theme)"},
    {"_id": 6, "title": "SpongeBob SquarePants Theme Song"},
    {"_id": 7, "title": "Call Me, Beep Me! (The Kim Possible Theme)"},
    {"_id": 8, "title": "My Heart Will Go On - Love Theme from \"Titanic\""},
    {"_id": 9, "title": "Rocky Theme"},
    {"_id": 10, "title": "Mia and Sebastian's Theme"},
    {"_id": 11, "title": "Barney's Theme Song"},
    {"_id": 12, "title": "So Long, London"},
    {"_id": 13, "title": "The Office Theme (from The Office)"},
    {"_id": 14, "title": "Theme from Jurassic Park"},
    {"_id": 15, "title": "Theme from Superman - Concert Version"},
    {"_id": 16, "title": "Ghostbusters - Instrumental Version"},
    {"_id": 17, "title": "Full House Theme"},
    {"_id": 18, "title": "The James Bond Theme"},
    {"_id": 19, "title": "Twelve Variations on Vous dirai-je, Mama"},
    {"_id": 20, "title": "Theme (from \"Spider Man\")"},
];
assert.commandWorked(coll.insertMany(docs));

const collUUID = getUUIDFromListCollections(db, coll.getName());
const mongotQuery = {
    query: "Theme",
    path: "title"
};

// All the documents that would be returned by the search query above.
let relevantDocs = [];
let relevantSearchDocs = [];
let relevantDocsNoId = [];
let searchScore = 0.300;
for (let i = 0; i < docs.length; i++) {
    if (docs[i]["title"].includes(mongotQuery.query)) {
        relevantDocs.push(docs[i]);
        relevantSearchDocs.push({_id: docs[i]._id, $searchScore: searchScore});
        relevantDocsNoId.push({title: docs[i].title})
    }

    // The documents with lower _id will have a higher search score.
    searchScore = searchScore - 0.001;
}

assert.eq(17, relevantDocs.length);

function mockRequest(batchSize) {
    const cursorId = NumberLong(99);
    const responseOk = 1;

    const history = [{
        expectedCommand: mongotCommandForQuery(mongotQuery,
                                               collName,
                                               dbName,
                                               collUUID,
                                               /*protocolVersion*/ null,
                                               /*cursorOptions*/ {batchSize}),
        response: mongotResponseForBatch(
            relevantSearchDocs.slice(0, batchSize), NumberLong(0), coll.getFullName(), responseOk),
    }];
    mongotMock.setMockResponses(history, cursorId);
}

/**
 * Mocks the mongot request / results for the given pipeline with the given expectedBatchSize,
 * then asserts that the results are correct.
 * This will fail (via the mongotMock internals) if the batchSize sent to mongot is different
 * than expected.
 */
function runBatchSizeExtractableLimitTest({pipeline, expectedBatchSize, expectedDocs}) {
    mockRequest(expectedBatchSize);
    let res = coll.aggregate(pipeline).toArray();
    assert.eq(expectedDocs, res);
}

runBatchSizeExtractableLimitTest({
    pipeline: [{$search: mongotQuery}, {$limit: 13}],
    expectedBatchSize: 13,
    expectedDocs: relevantDocs.slice(0, 13)
});

runBatchSizeExtractableLimitTest({
    pipeline: [{$search: mongotQuery}, {$limit: 17}],
    expectedBatchSize: 17,
    expectedDocs: relevantDocs
});

runBatchSizeExtractableLimitTest({
    pipeline: [{$search: mongotQuery}, {$limit: 20}],
    expectedBatchSize: 20,
    expectedDocs: relevantDocs
});

runBatchSizeExtractableLimitTest({
    pipeline: [{$search: mongotQuery}, {$limit: 500}],
    expectedBatchSize: 500,
    expectedDocs: relevantDocs
});

runBatchSizeExtractableLimitTest({
    pipeline: [{$search: mongotQuery}, {$limit: 500}, {$limit: 13}],
    expectedBatchSize: 13,
    expectedDocs: relevantDocs.slice(0, 13)
});

runBatchSizeExtractableLimitTest({
    pipeline: [{$search: mongotQuery}, {$project: {_id: 0}}, {$limit: 13}],
    expectedBatchSize: 13,
    expectedDocs: relevantDocsNoId.slice(0, 13)
});

// When we have an extractable limit less than 10, it should be rounded to minimum 10 when computing
// initial mongot batchSize.
runBatchSizeExtractableLimitTest({
    pipeline: [{$search: mongotQuery}, {$limit: 5}],
    expectedBatchSize: 10,
    expectedDocs: relevantDocs.slice(0, 5)
});

runBatchSizeExtractableLimitTest({
    pipeline: [{$search: mongotQuery}, {$limit: 13}, {$limit: 5}],
    expectedBatchSize: 10,
    expectedDocs: relevantDocs.slice(0, 5)
});

runBatchSizeExtractableLimitTest({
    pipeline:
        [{$search: mongotQuery}, {$limit: 7}, {$limit: 500}, {$project: {_id: 0}}, {$limit: 13}],
    expectedBatchSize: 10,
    expectedDocs: relevantDocsNoId.slice(0, 7)
});

runBatchSizeExtractableLimitTest({
    pipeline: [{$search: mongotQuery}, {$skip: 5}, {$limit: 10}],
    expectedBatchSize: 15,
    expectedDocs: relevantDocs.slice(5, 15)
});

runBatchSizeExtractableLimitTest({
    pipeline: [{$search: mongotQuery}, {$skip: 5}, {$limit: 3}, {$project: {_id: 0}}],
    expectedBatchSize: 10,
    expectedDocs: relevantDocsNoId.slice(5, 8)
});

runBatchSizeExtractableLimitTest({
    pipeline: [{$search: mongotQuery}, {$skip: 15}, {$limit: 100}],
    expectedBatchSize: 115,
    expectedDocs: relevantDocs.slice(15)
});

runBatchSizeExtractableLimitTest({
    pipeline: [{$search: mongotQuery}, {$skip: 5}, {$limit: 10}, {$skip: 5}, {$limit: 3}],
    expectedBatchSize: 13,
    expectedDocs: relevantDocs.slice(10, 13)
});

MongoRunner.stopMongod(conn);
mongotMock.stop();
