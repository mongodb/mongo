/**
 * End-to-end testing that the search index commands and the $listSearchIndexes aggregation stage
 * work on both mongos and mongod on a sharded cluster with more than 1 shard.
 */
import {assertErrCodeAndErrMsgContains} from "jstests/aggregation/extras/utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {prepCollection} from "jstests/with_mongot/mongotmock/lib/utils.js";

const mongotMock = new MongotMock();
mongotMock.start();
const mockConn = mongotMock.getConnection();

const st = new ShardingTest({
    mongos: 1,
    shards: 2,
    other: {
        mongosOptions: {setParameter: {searchIndexManagementHostAndPort: mockConn.host}},
        rsOptions: {setParameter: {searchIndexManagementHostAndPort: mockConn.host}},
    }
});

const mongos = st.s;
const dbName = jsTestName();
const collName = "testColl";
const testDB = mongos.getDB(dbName);
const testColl = testDB.getCollection(collName);
testColl.drop();

// We test the search index commands on a sharded and unsharded collection with a single shard in
// text_search_index_commands. This file will only have tests we expect to be affected by having
// multiple shards and the data spread over 2 shards. This is also why we are not going to test
// every possible syntax for each of the commands.

assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
prepCollection(mongos, dbName, collName);

// Shard the test collection, split it at {_id: 10}, and move the higher chunk to shard1.
st.shardColl(testColl, {_id: 1}, {_id: 10}, {_id: 10 + 1});

// The following tests will test the commands and $listSearchIndexes aggregation stage succeed.
(function createIndexSuccedsOneIndex() {
    const manageSearchIndexCommandResponse = {
        indexesCreated: [{id: "index-Id", name: "index-name"}]
    };

    mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
    assert.commandWorked(testDB.runCommand({
        'createSearchIndexes': collName,
        'indexes': [{'definition': {'mappings': {'dynamic': true}}, 'type': "vectorSearch"}]
    }));
})();

(function createIndexSuccedsMultipleIndexes() {
    const manageSearchIndexCommandResponse = {
        indexesCreated: [{id: "index-Id", name: "index-name"}]
    };

    mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
    assert.commandWorked(testDB.runCommand({
        'createSearchIndexes': collName,
        'indexes': [
            {'name': 'indexName', 'definition': {'mappings': {'dynamic': true}}},
            {'definition': {'mappings': {'dynamic': false}}},
        ]
    }));
})();

(function updateIndexSucceeds() {
    const manageSearchIndexCommandResponse = {ok: 1};
    mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
    assert.commandWorked(testDB.runCommand({
        'updateSearchIndex': collName,
        'id': 'index-ID-number',
        'definition': {"testBlob": "blob"}
    }));
})();

(function dropIndexSucceeds() {
    const manageSearchIndexCommandResponse = {ok: 1};
    mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
    assert.commandWorked(testDB.runCommand({'dropSearchIndex': collName, 'name': 'indexName'}));
})();

(function listSearchIndexCommandSucceeds() {
    const manageSearchIndexCommandResponse = {
        ok: 1,
        cursor: {
            id: 0,
            ns: "database-name.collection-name",
            firstBatch: [
                {
                    id: "index-Id",
                    name: "index-name",
                    status: "INITIAL-SYNC",
                    definition: {
                        mappings: {
                            dynamic: true,
                        }
                    },
                },
                {
                    id: "index-Id",
                    name: "index-name",
                    status: "ACTIVE",
                    definition: {
                        mappings: {
                            dynamic: true,
                        },
                        synonyms: [{"synonym-mapping": "thing"}],
                    }
                }
            ]
        }
    };

    mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
    assert.commandWorked(testDB.runCommand({'listSearchIndexes': collName}));
})();

(function listSearchIndexCommandSucceeds_EmptyResponse() {
    const emptyResponse = {
        ok: 1,
        cursor: {id: 0, ns: "database-name.collection-name", firstBatch: []}
    };
    mongotMock.setMockSearchIndexCommandResponse(emptyResponse);
    assert.commandWorked(testDB.runCommand({'listSearchIndexes': collName}));
})();

(function listSearchIndexAggStageSucceeds() {
    const manageSearchIndexCommandResponse = {
        ok: 1,
        cursor: {
            id: 0,
            ns: "database-name.collection-name",
            firstBatch: [
                {
                    id: "index-Id",
                    name: "index-name",
                    status: "INITIAL-SYNC",
                    definition: {
                        mappings: {
                            dynamic: true,
                        }
                    },
                },
                {
                    id: "index-Id",
                    name: "index-name",
                    status: "ACTIVE",
                    definition: {
                        mappings: {
                            dynamic: true,
                        },
                        synonyms: [{"synonym-mapping": "thing"}],
                    }
                }
            ]
        }
    };

    const expectedDocs = manageSearchIndexCommandResponse["cursor"]["firstBatch"];
    mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
    const result = testColl.aggregate([{$listSearchIndexes: {}}]).toArray();
    assert.eq(result, expectedDocs);
}());

(function listSearchIndexAggStageSucceeds_EmptyResponse() {
    const emptyResponse = {
        ok: 1,
        cursor: {id: 0, ns: "database-name.collection-name", firstBatch: []}
    };
    mongotMock.setMockSearchIndexCommandResponse(emptyResponse);
    const expectedDocs = emptyResponse["cursor"]["firstBatch"];
    const result =
        testColl.aggregate([{$listSearchIndexes: {}}], {cursor: {batchSize: 1}}).toArray();
    assert.eq(result, expectedDocs);
})();

// The following tests will validate that a search index management server error propagates back
// through the sharded collection correctly.
const manageSearchIndexCommandResponse = {
    ok: 0,
    errmsg: "create failed due to malformed index (pretend)",
    code: 207,
    // Choose a different error than what the search commands can return to ensure the
    // error gets passed through.
    codeName: "InvalidUUID",
};

(function createIndexError() {
    mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
    let res = assert.commandFailedWithCode(testDB.runCommand({
        'createSearchIndexes': collName,
        'indexes': [
            {'name': 'indexName', 'definition': {'mappings': {'dynamic': true}}},
            {'definition': {'mappings': {'dynamic': false}}},
        ],
    }),
                                           ErrorCodes.InvalidUUID);
    delete res.$clusterTime;
    delete res.operationTime;
    assert.eq(manageSearchIndexCommandResponse, res);
})();

(function dropIndexError() {
    mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
    let res = assert.commandFailedWithCode(
        testDB.runCommand({'dropSearchIndex': collName, 'name': 'indexName'}),
        ErrorCodes.InvalidUUID);
    delete res.$clusterTime;
    delete res.operationTime;
    assert.eq(manageSearchIndexCommandResponse, res);
})();

(function updateIndexError() {
    mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
    let res = assert.commandFailedWithCode(testDB.runCommand({
        'updateSearchIndex': collName,
        'id': 'index-ID-number',
        'definition': {"testBlob": "blob"},
    }),
                                           ErrorCodes.InvalidUUID);
    delete res.$clusterTime;
    delete res.operationTime;
    assert.eq(manageSearchIndexCommandResponse, res);
})();

// The listSearchIndex command and agg stage can also return a IndexInformationTooLarge error.
const indexInformationTooLargeError = {
    ok: 0,
    errmsg: "the search index information for this collection exceeds 16 MB",
    code: 396,
    codeName: "IndexInformationTooLarge",
};

(function listSearchIndexCommandError() {
    mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
    let res = assert.commandFailedWithCode(testDB.runCommand({'listSearchIndexes': collName}),
                                           ErrorCodes.InvalidUUID);
    delete res.$clusterTime;
    delete res.operationTime;
    assert.eq(manageSearchIndexCommandResponse, res);

    // Test returning a IndexInformationTooLarge error.
    mongotMock.setMockSearchIndexCommandResponse(indexInformationTooLargeError);
    res = assert.commandFailedWithCode(testDB.runCommand({'listSearchIndexes': collName}),
                                       ErrorCodes.IndexInformationTooLarge);
    delete res.$clusterTime;
    delete res.operationTime;
    assert.eq(indexInformationTooLargeError, res);
})();

(function listSearchIndexAggStageError() {
    // Aggregation errors add additional context to the error message, so we use a helper
    // function to validate the error message.
    mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
    let pipeline = [{'$listSearchIndexes': {'id': 'indexID'}}];
    let expectErrMsg = manageSearchIndexCommandResponse["errmsg"];
    let expectedCode = manageSearchIndexCommandResponse["code"];
    assertErrCodeAndErrMsgContains(testDB[collName], pipeline, expectedCode, expectErrMsg);

    // Test returning a IndexInformationTooLarge error.
    mongotMock.setMockSearchIndexCommandResponse(indexInformationTooLargeError);
    pipeline = [{'$listSearchIndexes': {'id': 'indexID'}}];
    expectErrMsg = indexInformationTooLargeError["errmsg"];
    expectedCode = indexInformationTooLargeError["code"];
    assertErrCodeAndErrMsgContains(testDB[collName], pipeline, expectedCode, expectErrMsg);
})();

// Test all the commands fail if the collection doesn't exist, but the host is setup.
// $listSearchIndexes aggregation stage should return a empty result set.
(function testCollectionDoesNotExistTest() {
    const droppedCollName = "dropped_coll";
    const droppedColl = testDB.getCollection(droppedCollName);
    droppedColl.drop();

    assert.commandFailedWithCode(testDB.runCommand({
        'createSearchIndexes': droppedCollName,
        'indexes': [{'definition': {'mappings': {'dynamic': true}}}]
    }),
                                 ErrorCodes.NamespaceNotFound);

    assert.commandFailedWithCode(testDB.runCommand({
        'updateSearchIndex': droppedCollName,
        'id': 'index-ID-number',
        'definition': {"testBlob": "blob"}
    }),
                                 ErrorCodes.NamespaceNotFound);

    assert.commandFailedWithCode(
        testDB.runCommand({'dropSearchIndex': droppedCollName, 'name': 'indexName'}),
        ErrorCodes.NamespaceNotFound);

    assert.commandFailedWithCode(testDB.runCommand({'listSearchIndexes': droppedCollName}),
                                 ErrorCodes.NamespaceNotFound);

    // The $listSearchIndexes aggregation stage should return an empty result set rather than an
    // error when the collection doesn't exist and the port is set up.
    const result = testDB[droppedColl].aggregate([{$listSearchIndexes: {}}]).toArray();
    assert.eq([], result, "Expected non-existent collection to return an empty result set");
}());

st.stop();
mongotMock.stop();
