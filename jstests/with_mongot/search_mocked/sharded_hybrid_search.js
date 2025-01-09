/**
 * This test is designed to reproduce an issue (SERVER-96412) involving a query over two collections
 * which is similar to a hybrid search pattern.
 *
 * The scenario involves a single-shard sharded cluster, and an aggregation with a $unionWith.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    getDefaultProtocolVersionForPlanShardedSearch,
    mongotCommandForQuery,
    mongotCommandForVectorSearchQuery,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const stWithMock = new ShardingTestWithMongotMock({shards: {rs0: {nodes: 1}}, mongos: 1});
stWithMock.start();
const st = stWithMock.st;

const dbName = jsTestName();

const mongos = st.s;
const testDB = mongos.getDB(dbName);
const protocolVersion = getDefaultProtocolVersionForPlanShardedSearch();

const searchColl = testDB.getCollection("search_sharded");
const vectorColl = testDB.getCollection("vector_sharded");

const vectorCollDocs = [
    {"_id": 100, "title": "cakes", vector: [1, 2], "weird": false},
    {"_id": 101, "title": "cakes and kale", vector: [1, 1], "weird": true},
];

const searchCollDocs = [
    {"_id": 0, "title": "cakes"},
    {"_id": 1, "title": "cookies and cakes"},
    {"_id": 2, "title": "vegetables"},
    {"_id": 3, "title": "oranges"},
    {"_id": 4, "title": "cakes and oranges"},
    {"_id": 5, "title": "cakes and apples"},
    {"_id": 6, "title": "apples"},
    {"_id": 7, "title": "cakes and kale"},
];

function loadData(coll, docs) {
    coll.drop();
    assert.commandWorked(coll.insertMany(docs));
    // We want to run a variant of this test using 'title' as a shard key. An index is required to
    // shard the collection.
    assert.commandWorked(coll.createIndex({title: 1}));
}

const sMongot = stWithMock.getMockConnectedToHost(stWithMock.st.s);
const d0Mongot = stWithMock.getMockConnectedToHost(st.rs0.getPrimary());

const searchQuery = {
    query: "cakes",
    path: "title"
};

const vectorSearchQuery = {
    queryVector: [0, 0],
    path: "vector",
    numCandidates: 2,
    index: "vector_search",
    limit: 2,
};

//------------------------
// Define mocks' responses
//------------------------

function searchQueryExpectedByMock(searchColl, protocolVersion = null) {
    return mongotCommandForQuery({
        query: searchQuery,
        collName: searchColl.getName(),
        db: testDB.getName(),
        collectionUUID: getUUIDFromListCollections(testDB, searchColl.getName()),
        protocolVersion: protocolVersion
    });
}

function vectorSearchQueryExpectedByMock(coll) {
    const db = testDB.getName();
    const collUUID = getUUIDFromListCollections(testDB, coll.getName());
    return mongotCommandForVectorSearchQuery(
        {...vectorSearchQuery, collName: coll.getName(), dbName, collectionUUID: collUUID});
}

// What mongot should give back:
const mockedSearchResultIds =
    [{_id: 4, $searchScore: 0.45}, {_id: 5, $searchScore: 0.38}, {_id: 7, $searchScore: 0.33}];
// What we expect after idLookup:
const mockedSearchResults = [
    {"_id": 4, "title": "cakes and oranges"},
    {"_id": 5, "title": "cakes and apples"},
    {"_id": 7, "title": "cakes and kale"},
];
// Small note: this is a function and not a constant because the UUID is not known until runtime, so
// we need this to happen after the collection is set up.
const searchMock = () => [{
    expectedCommand: searchQueryExpectedByMock(searchColl),
    response: {
        ok: 1,
        cursor: {firstBatch: mockedSearchResultIds, id: NumberLong(0), ns: searchColl.getFullName()}
    }
}];

// What mongot should give back:
const mockedVectorSearchResultIds =
    [{_id: 100, $vectorSearchScore: 0.45}, {_id: 101, $vectorSearchScore: 0.41}];
// What we expect after idLookup:
const mockedVectorSearchResults = [
    {"_id": 100, "title": "cakes", vector: [1, 2], "weird": false},
    {"_id": 101, "title": "cakes and kale", vector: [1, 1], "weird": true},
];
// Small note: this is a function and not a constant because the UUID is not known until runtime, so
// we need this to happen after the collection is set up.
const vectorSearchMock = () => [{
    expectedCommand: vectorSearchQueryExpectedByMock(vectorColl),
    response: {
        ok: 1,
        cursor: {
            firstBatch: mockedVectorSearchResultIds,
            ns: vectorColl.getFullName(),
            id: NumberLong(0),
        }
    },
}];

const planShardedSearchMock = [{
    expectedCommand: {
        planShardedSearch: searchColl.getName(),
        query: searchQuery,
        $db: dbName,
        searchFeatures: {shardedSort: 1}
    },
    response: {
        ok: 1,
        protocolVersion: protocolVersion,
        metaPipeline:  // Sum counts in the shard metadata.
            [{$group: {_id: null, count: {$sum: "$count"}}}, {$project: {_id: 0, count: 1}}]
    },
}];

let cursorId = 1000;
function setupMockRequest({mongotMockConn, mockCmdAndResponse}) {
    mongotMockConn.setMockResponses(mockCmdAndResponse, cursorId);
    cursorId += 1;
}

//---------------------------------------------
// Define test cases, with corresponding mocks.
//---------------------------------------------

const unionWithSearch = {
    $unionWith: {coll: searchColl.getName(), pipeline: [{$search: searchQuery}]}
};

function testUnionWithSearch() {
    const pipeline = [unionWithSearch];
    setupMockRequest({mongotMockConn: d0Mongot, mockCmdAndResponse: searchMock()});
    const results = vectorColl.aggregate(pipeline).toArray();
    const expectedResults = vectorCollDocs.concat(mockedSearchResults);
    assert.sameMembers(results, expectedResults);
}

const unionWithVector = {
    $unionWith: {coll: vectorColl.getName(), pipeline: [{$vectorSearch: vectorSearchQuery}]}
};
function testUnionWithVector() {
    const pipeline = [unionWithVector];
    setupMockRequest({mongotMockConn: d0Mongot, mockCmdAndResponse: vectorSearchMock()});
    const expectedUnionWithResult = searchCollDocs.concat(mockedVectorSearchResults);
    const results = searchColl.aggregate(pipeline).toArray();
    assert.sameMembers(results, expectedUnionWithResult);
}

function testVectorUnionWithSearch() {
    const pipeline = [
        {$vectorSearch: vectorSearchQuery},
        unionWithSearch,
    ];
    setupMockRequest({mongotMockConn: d0Mongot, mockCmdAndResponse: vectorSearchMock()});
    setupMockRequest({mongotMockConn: d0Mongot, mockCmdAndResponse: searchMock()});
    const expectedResult = mockedVectorSearchResults.concat(mockedSearchResults);
    const results = vectorColl.aggregate(pipeline).toArray();
    assert.sameMembers(results, expectedResult);
}

function testSearchUnionWithVector() {
    const pipeline = [
        {$search: searchQuery},
        unionWithVector,
    ];
    setupMockRequest({mongotMockConn: sMongot, mockCmdAndResponse: planShardedSearchMock});
    setupMockRequest({mongotMockConn: d0Mongot, mockCmdAndResponse: searchMock()});
    setupMockRequest({mongotMockConn: d0Mongot, mockCmdAndResponse: vectorSearchMock()});
    const expectedResults = mockedSearchResults.concat(mockedVectorSearchResults);
    const actualResults = searchColl.aggregate(pipeline).toArray();
    assert.sameMembers(actualResults, expectedResults);
}

const lookupSearch = {
    $lookup: {from: searchColl.getName(), pipeline: [{$search: searchQuery}], as: "lookup_results"}
};
function testLookupSearch() {
    const pipeline = [lookupSearch];
    // You might expect to need 'vectorCollDocs.length' mocked responses here, but
    // because the pipeline is uncorrelated, it can be cached and thus executed only once.
    setupMockRequest({mongotMockConn: d0Mongot, mockCmdAndResponse: searchMock()});
    const expectedResults =
        vectorCollDocs.map(res => Object.merge(res, {lookup_results: mockedSearchResults}));
    const actualResults = vectorColl.aggregate(pipeline).toArray();
    assert.sameMembers(actualResults, expectedResults);
}
function testVectorLookupSearch() {
    const pipeline = [{$vectorSearch: vectorSearchQuery}, lookupSearch];
    setupMockRequest({mongotMockConn: d0Mongot, mockCmdAndResponse: vectorSearchMock()});
    // You might expect to need 'mockedVectorSearchResults.length' mocked responses here, but
    // because the pipeline is uncorrelated, it can be cached and thus executed only once.
    setupMockRequest({mongotMockConn: d0Mongot, mockCmdAndResponse: searchMock()});
    const expectedResults = mockedVectorSearchResults.map(
        res => Object.merge(res, {lookup_results: mockedSearchResults}));
    const actualResults = vectorColl.aggregate(pipeline).toArray();
    assert.sameMembers(actualResults, expectedResults);
}

function runTest(testFn) {
    testFn();
    stWithMock.assertEmptyMocks();
}

function testAll() {
    runTest(testUnionWithSearch);
    runTest(testUnionWithVector);
    runTest(testVectorUnionWithSearch);
    runTest(testSearchUnionWithVector);
    runTest(testLookupSearch);
    runTest(testVectorLookupSearch);
    // TODO SERVER-88602 $vectorSearch is not supported inside a $lookup. That would be interesting
    // coverage to add here.
}

//-----------------------
// Actually do the tests.
//-----------------------

function setup({shardKey} = {}) {
    loadData(vectorColl, vectorCollDocs);
    loadData(searchColl, searchCollDocs);

    if (shardKey !== undefined) {
        st.shardColl(searchColl, shardKey, /* split or move any chunks= */ false);
        st.shardColl(vectorColl, shardKey, /* split or move any chunks= */ false);
    }
}

setup();
testAll();
setup({shardKey: {title: 1}});
testAll();
setup({shardKey: {_id: 1}});
testAll();

stWithMock.stop();
