/**
 * End-to-end testing that the search index commands and the $listSearchIndexes aggregation stage
 * work on both mongos and mongod.
 */

import {assertErrCodeAndErrMsgContains} from "jstests/aggregation/extras/utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

const dbName = jsTestName();
const shardedCollName = "testColl";
const unshardedCollName = shardedCollName + "_unsharded";

let unavailableHostAndPort;

// Test the mock search index management server works.
{
    const mongotMock = new MongotMock();
    mongotMock.start();

    const testResponse = {
        someField: 'someFieldValue',
    };

    // Set up a mock response to the 'manageSearchIndex' command.
    mongotMock.setMockSearchIndexCommandResponse(testResponse);

    // Call the 'manageSearchIndex' command and check it returns the response that was set.
    const response = mongotMock.callManageSearchIndexCommand();
    // The response includes ok:1, so check the field specifically.
    assert.eq(testResponse.someField, response.someField);

    // Save this for later testing of an unavailable remote server.
    unavailableHostAndPort = mongotMock.getConnection().host;

    mongotMock.stop();
}

// Test that the search index commands are not supported if the 'searchIndexManagementHostAndPort'
// server parameter is not set. Initializing the server parameter conveys that the server is running
// with search index management and the search index commands are supported.
{
    const runHostAndPortNotSetTest = function(conn) {
        const testDB = conn.getDB(dbName);
        assert.commandFailedWithCode(testDB.runCommand({
            'createSearchIndexes': shardedCollName,
            'indexes': [{'definition': {'mappings': {'dynamic': true}}}]
        }),
                                     ErrorCodes.SearchNotEnabled);

        assert.commandFailedWithCode(testDB.runCommand({
            'updateSearchIndex': shardedCollName,
            'id': 'index-ID-number',
            'definition': {"testBlob": "blob"}
        }),
                                     ErrorCodes.SearchNotEnabled);

        assert.commandFailedWithCode(
            testDB.runCommand({'dropSearchIndex': shardedCollName, 'name': 'indexName'}),
            ErrorCodes.SearchNotEnabled);

        assert.commandFailedWithCode(testDB.runCommand({'listSearchIndexes': shardedCollName}),
                                     ErrorCodes.SearchNotEnabled);

        assert.commandFailedWithCode(
            testDB.runCommand(
                {aggregate: shardedCollName, pipeline: [{$listSearchIndexes: {}}], cursor: {}}),
            ErrorCodes.SearchNotEnabled);

        // For the $listSearchIndexes aggregation stage, the 'SearchNotEnabled' error should be
        // thrown before parsing errors.
        assert.commandFailedWithCode(testDB.runCommand({
            aggregate: shardedCollName,
            pipeline: [{$listSearchIndexes: {"unknown": 1}}],
            cursor: {}
        }),
                                     ErrorCodes.SearchNotEnabled);
    };
    let st = new ShardingTest({
        mongos: 1,
        shards: 1,
    });
    // Must create the collection for the aggregation stage to fail. Otherwise empty results are
    // returned by mongos, but mongod will still throw 'SearchNotEnabled' if the collection
    // doesn't exist.
    const testDB = st.s.getDB(dbName);
    assert.commandWorked(testDB.createCollection(shardedCollName));
    // Test the mongos search index commands.
    runHostAndPortNotSetTest(st.s);
    // Test the mongod search index commands.
    runHostAndPortNotSetTest(st.shard0);
    st.stop();
}

// Test that $listSearchIndexes throws an error if the 'searchIndexManagementHostAndPort' server
// parameter is not set and the database does not exist.
{
    const runHostAndPortNotSetDbNotExistTest = function(conn) {
        const testDB = conn.getDB(dbName);
        assert.commandWorked(testDB.dropDatabase());
        assert.commandFailedWithCode(
            testDB.runCommand(
                {aggregate: "coll", pipeline: [{$listSearchIndexes: {}}], cursor: {}}),
            ErrorCodes.SearchNotEnabled);
    };
    let st = new ShardingTest({
        mongos: 1,
        shards: 1,
    });
    runHostAndPortNotSetDbNotExistTest(st.s);
    runHostAndPortNotSetDbNotExistTest(st.shard0);
    st.stop();
}

// Test that $listSearchIndexes throws an error if the 'searchIndexManagementHostAndPort' server
// parameter is not set and the collection does not exist.
{
    const runHostAndPortNotSetCollNotExistTest = function(conn) {
        const testDB = conn.getDB(dbName);
        // Create another collection to ensure the database exists.
        assert.commandWorked(testDB.createCollection(shardedCollName));
        assert.commandFailedWithCode(
            testDB.runCommand(
                {aggregate: unshardedCollName, pipeline: [{$listSearchIndexes: {}}], cursor: {}}),
            ErrorCodes.SearchNotEnabled);
    };
    let st = new ShardingTest({
        mongos: 1,
        shards: 1,
    });
    runHostAndPortNotSetCollNotExistTest(st.s);
    runHostAndPortNotSetCollNotExistTest(st.shard0);
    st.stop();
}

// Test that the mongod search index commands fail when the remote search index management server is
// not reachable. Set a host-and-port for the remote server that is not live in order to simulate
// unreachability.
{
    const runHostAndPortUnreachableTest = function(conn) {
        const testDB = conn.getDB(dbName);
        assert.commandFailedWithCode(testDB.runCommand({
            'createSearchIndexes': shardedCollName,
            'indexes': [{'definition': {'mappings': {'dynamic': true}}}]
        }),
                                     ErrorCodes.CommandFailed);

        // The code to reach the remote search index management server is shared across search index
        // commands. No need to test all of the commands, but we will test the $listSearchIndexes
        // aggregation stage.
        assert.commandFailedWithCode(
            testDB.runCommand(
                {aggregate: shardedCollName, pipeline: [{$listSearchIndexes: {}}], cursor: {}}),
            ErrorCodes.CommandFailed);
    };

    let st = new ShardingTest({
        mongos: 1,
        shards: 1,
        other: {
            mongosOptions:
                {setParameter: {searchIndexManagementHostAndPort: unavailableHostAndPort}},
            rsOptions: {setParameter: {searchIndexManagementHostAndPort: unavailableHostAndPort}},
        }
    });
    const mongos = st.s;
    const testDBMongos = mongos.getDB(dbName);
    const testCollMongos = testDBMongos.getCollection(shardedCollName);

    // Create and shard the test collection so the commands can succeed locally.
    assert.commandWorked(testDBMongos.createCollection(shardedCollName));
    assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
    assert.commandWorked(
        mongos.adminCommand({shardCollection: testCollMongos.getFullName(), key: {a: 1}}));

    runHostAndPortUnreachableTest(mongos);
    runHostAndPortUnreachableTest(st.shard0);

    st.stop();
}

const mongotMock = new MongotMock();
mongotMock.start();
const mockConn = mongotMock.getConnection();

// Test that the mongod links in the mongod-only command logic, not the mongos logic that asks the
// config server for the collection UUID.
{
    const conn =
        MongoRunner.runMongod({setParameter: {searchIndexManagementHostAndPort: mockConn.host}});
    assert(conn);
    const testDB = conn.getDB(dbName);

    // Create the collection so the commands can succeed on the mongod.
    assert.commandWorked(testDB.createCollection(shardedCollName));

    const manageSearchIndexCommandResponse = {
        indexesCreated: [{id: "index-Id", name: "index-name"}]
    };

    mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
    assert.commandWorked(testDB.runCommand({
        'createSearchIndexes': shardedCollName,
        'indexes': [{'definition': {'mappings': {'dynamic': true}}}]
    }));

    MongoRunner.stopMongod(conn);
}

let st = new ShardingTest({
    mongos: 1,
    shards: 1,
    other: {
        mongosOptions: {setParameter: {searchIndexManagementHostAndPort: mockConn.host}},
        rs0: {
            // Need the shard to have a stable secondary to test commands against.
            nodes: [{}, {rsConfig: {priority: 0}}],
            setParameter: {searchIndexManagementHostAndPort: mockConn.host},
        },
    }
});

const mongos = st.s;
const testDBMongos = mongos.getDB(dbName);
const testCollMongos = testDBMongos.getCollection(shardedCollName);

// Test that the commands all fail if the collection does not exist when the port is set up.
{
    const runCollectionDoesNotExistTest = function(conn) {
        const testDB = conn.getDB(dbName);

        assert.commandFailedWithCode(testDB.runCommand({
            'createSearchIndexes': shardedCollName,
            'indexes': [{'definition': {'mappings': {'dynamic': true}}}]
        }),
                                     ErrorCodes.NamespaceNotFound);

        assert.commandFailedWithCode(testDB.runCommand({
            'updateSearchIndex': shardedCollName,
            'id': 'index-ID-number',
            'definition': {"testBlob": "blob"}
        }),
                                     ErrorCodes.NamespaceNotFound);

        assert.commandFailedWithCode(
            testDB.runCommand({'dropSearchIndex': shardedCollName, 'name': 'indexName'}),
            ErrorCodes.NamespaceNotFound);

        assert.commandFailedWithCode(testDB.runCommand({'listSearchIndexes': shardedCollName}),
                                     ErrorCodes.NamespaceNotFound);

        // The $listSearchIndexes aggregation stage should return an empty result set rather than an
        // error when the collection doesn't exist and the port is set up.
        const result = testDB[shardedCollName].aggregate([{$listSearchIndexes: {}}]).toArray();
        assert.eq([], result, "Expected non-existent collection to return an empty result set");
    };
    runCollectionDoesNotExistTest(st.s);
    runCollectionDoesNotExistTest(st.shard0);
}

// Create and shard the collection so the commands can succeed.
assert.commandWorked(testDBMongos.createCollection(shardedCollName));
assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
assert.commandWorked(
    mongos.adminCommand({shardCollection: testCollMongos.getFullName(), key: {a: 1}}));

// Create an unsharded collection.
assert.commandWorked(testDBMongos.createCollection(unshardedCollName));
st.awaitReplicationOnShards();

function testAgainstCollection(collName) {
    // Test that the search index commands fail against a secondary replica set member. However,
    // $listSearchIndexes aggregation stage should succeed.
    {
        const secondaryDB = st.rs0.getSecondary().getDB(dbName);

        assert.commandFailedWithCode(secondaryDB.runCommand({
            'createSearchIndexes': collName,
            'indexes': [{'definition': {'mappings': {'dynamic': true}}}]
        }),
                                     ErrorCodes.NotWritablePrimary);

        assert.commandFailedWithCode(secondaryDB.runCommand({
            'updateSearchIndex': collName,
            'id': 'index-ID-number',
            'definition': {"testBlob": "blob"}
        }),
                                     ErrorCodes.NotWritablePrimary);

        assert.commandFailedWithCode(
            secondaryDB.runCommand({'dropSearchIndex': collName, 'name': 'indexName'}),
            ErrorCodes.NotWritablePrimary);

        assert.commandFailedWithCode(secondaryDB.runCommand({'listSearchIndexes': collName}),
                                     ErrorCodes.NotWritablePrimary);

        // The aggregation stage should succeed against secondaries.
        const manageSearchIndexCommandResponse = {
            ok: 1,
            cursor: {
                id: 0,
                ns: "database-name.collection-name",
                firstBatch: [{
                    id: "index-Id",
                    name: "index-name",
                    status: "INITIAL-SYNC",
                    definition: {
                        mappings: {
                            dynamic: true,
                        }
                    },
                }]
            }
        };
        mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
        assert.commandWorked(secondaryDB.runCommand(
            {aggregate: collName, pipeline: [{$listSearchIndexes: {}}], cursor: {}}));
    }

    // Test creating search indexes.
    {
        const runCreateSearchIndexesTest = function(conn) {
            const testDB = conn.getDB(dbName);
            const manageSearchIndexCommandResponse = {
                indexesCreated: [{id: "index-Id", name: "index-name"}]
            };

            // Test with type 'search'.
            mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
            assert.commandWorked(testDB.runCommand({
                'createSearchIndexes': collName,
                'indexes': [{'definition': {'mappings': {'dynamic': true}}, 'type': "search"}]
            }));

            // Test with type 'vectorSearch'.
            mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
            assert.commandWorked(testDB.runCommand({
                'createSearchIndexes': collName,
                'indexes': [{'definition': {'mappings': {'dynamic': true}}, 'type': "vectorSearch"}]
            }));

            // Test with no type.
            mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
            assert.commandWorked(testDB.runCommand({
                'createSearchIndexes': collName,
                'indexes': [{'definition': {'mappings': {'dynamic': true}}}]
            }));

            // Test with incorrect type.
            const incorrectTypeError = {
                ok: 0,
                // Note that this is not the real error code, msg, or name from mongot. We pick a
                // random number/code to ensure the error propagates.
                errmsg: "create failed due to invalid index type",
                code: 207,
                codeName: "InvalidType",
            };
            mongotMock.setMockSearchIndexCommandResponse(incorrectTypeError);
            assert.commandFailed(testDB.runCommand({
                'createSearchIndexes': collName,
                'indexes':
                    [{'definition': {'mappings': {'dynamic': true}}, 'type': "nonsenseIndex"}]
            }));

            mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
            assert.commandWorked(testDB.runCommand({
                'createSearchIndexes': collName,
                'indexes': [{'name': 'indexName', 'definition': {'mappings': {'dynamic': true}}}]
            }));

            mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
            assert.commandWorked(testDB.runCommand({
                'createSearchIndexes': collName,
                'indexes': [
                    {'name': 'indexName', 'definition': {'mappings': {'dynamic': true}}},
                    {'definition': {'mappings': {'dynamic': false}}},
                ]
            }));
        };
        runCreateSearchIndexesTest(mongos);
        runCreateSearchIndexesTest(st.shard0);
    }

    // Test updating search indexes.
    {
        const runUpdateSearchIndexTest = function(conn) {
            const testDB = conn.getDB(dbName);
            const manageSearchIndexCommandResponse = {ok: 1};

            mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
            assert.commandWorked(testDB.runCommand({
                'updateSearchIndex': collName,
                'id': 'index-ID-number',
                'definition': {"testBlob": "blob"}
            }));

            mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
            assert.commandWorked(testDB.runCommand({
                'updateSearchIndex': collName,
                'name': 'indexName',
                'definition': {"testBlob": "blob"}
            }));

            // Cannot run update without specifying what index to update by 'name' or 'id'.
            assert.commandFailedWithCode(
                testDB.runCommand(
                    {'updateSearchIndex': collName, 'definition': {"testBlob": "blob"}}),
                ErrorCodes.InvalidOptions);

            // Not allowed to run update specifying both 'name' and 'id'.
            assert.commandFailedWithCode(testDB.runCommand({
                'updateSearchIndex': collName,
                'name': 'indexName',
                'id': 'index-ID-number',
                'definition': {"testBlob": "blob"}
            }),
                                         ErrorCodes.InvalidOptions);
        };
        runUpdateSearchIndexTest(st.s);
        runUpdateSearchIndexTest(st.shard0);
    }

    // Test dropping search indexes.
    {
        const runDropSearchIndexTest = function(conn) {
            const testDB = conn.getDB(dbName);
            const manageSearchIndexCommandResponse = {ok: 1};

            mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
            assert.commandWorked(
                testDB.runCommand({'dropSearchIndex': collName, 'name': 'indexName'}));

            mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
            assert.commandWorked(
                testDB.runCommand({'dropSearchIndex': collName, 'id': 'index-ID-number'}));

            // Not allowed to run drop specifying both 'name' and 'id'.
            assert.commandFailedWithCode(
                testDB.runCommand(
                    {'dropSearchIndex': collName, 'name': 'indexName', 'id': 'index-ID-number'}),
                ErrorCodes.InvalidOptions);
        };
        runDropSearchIndexTest(st.s);
        runDropSearchIndexTest(st.shard0);
    }

    // Test listing the search indexes.
    {
        const runListSearchIndexesTest = function(conn) {
            const testDB = conn.getDB(dbName);

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

            mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
            assert.commandWorked(
                testDB.runCommand({'listSearchIndexes': collName, 'name': 'indexName'}));

            mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
            assert.commandWorked(
                testDB.runCommand({'listSearchIndexes': collName, 'id': 'index-ID-number'}));

            // Not allowed to run list specifying both 'name' and 'id'.
            assert.commandFailedWithCode(
                testDB.runCommand(
                    {'listSearchIndexes': collName, 'name': 'indexName', 'id': 'index-ID-number'}),
                ErrorCodes.InvalidOptions);
        };
        runListSearchIndexesTest(st.s);
        runListSearchIndexesTest(st.shard0);
    }

    // Test the $listSearchIndexes aggregation stage.
    {
        const runListSearchIndexesAggTest = function(conn) {
            const testDB = conn.getDB(dbName);
            const coll = testDB.getCollection(collName);
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
            let result =
                coll.aggregate([{$listSearchIndexes: {}}], {cursor: {batchSize: 1}}).toArray();
            let expectedDocs = manageSearchIndexCommandResponse["cursor"]["firstBatch"];
            assert.eq(result, expectedDocs);

            mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
            result = coll.aggregate([{$listSearchIndexes: {'name': 'index-name'}}],
                                    {cursor: {batchSize: 1}})
                         .toArray();
            assert.eq(result, expectedDocs);

            mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
            result =
                coll.aggregate([{$listSearchIndexes: {'id': 'index-Id'}}], {cursor: {batchSize: 1}})
                    .toArray();
            assert.eq(result, expectedDocs);

            // Test that the aggregation stage handles an empty response from 'manageSearchIndex'.
            const emptyResponse = {
                ok: 1,
                cursor: {id: 0, ns: "database-name.collection-name", firstBatch: []}
            };
            mongotMock.setMockSearchIndexCommandResponse(emptyResponse);
            expectedDocs = emptyResponse["cursor"]["firstBatch"];
            result = coll.aggregate([{$listSearchIndexes: {}}], {cursor: {batchSize: 1}}).toArray();
            assert.eq(result, expectedDocs);

            // Not allowed to run list specifying both 'name' and 'id'.
            assert.commandFailedWithCode(testDB.runCommand(({
                aggregate: collName,
                pipeline: [{'$listSearchIndexes': {'name': 'indexName', 'id': 'indexID'}}],
                cursor: {}
            })),
                                         ErrorCodes.InvalidOptions);
        };
        runListSearchIndexesAggTest(st.s);
        runListSearchIndexesAggTest(st.shard0);
    }

    // Test that a search index management server error propagates back through the mongod
    // correctly.
    {
        const runRemoteSearchIndexManagementServerErrorsTest = function(conn) {
            const testDB = conn.getDB(dbName);
            const manageSearchIndexCommandResponse = {
                ok: 0,
                errmsg: "create failed due to malformed index (pretend)",
                code: 207,
                // Choose a different error than what the search commands can return to ensure the
                // error gets passed through.
                codeName: "InvalidUUID",
            };

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

            mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
            res = assert.commandFailedWithCode(
                testDB.runCommand({'dropSearchIndex': collName, 'name': 'indexName'}),
                ErrorCodes.InvalidUUID);
            delete res.$clusterTime;
            delete res.operationTime;
            assert.eq(manageSearchIndexCommandResponse, res);

            mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
            res = assert.commandFailedWithCode(testDB.runCommand({
                'updateSearchIndex': collName,
                'id': 'index-ID-number',
                'definition': {"testBlob": "blob"},
            }),
                                               ErrorCodes.InvalidUUID);
            delete res.$clusterTime;
            delete res.operationTime;
            assert.eq(manageSearchIndexCommandResponse, res);

            mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
            res = assert.commandFailedWithCode(testDB.runCommand({'listSearchIndexes': collName}),
                                               ErrorCodes.InvalidUUID);
            delete res.$clusterTime;
            delete res.operationTime;
            assert.eq(manageSearchIndexCommandResponse, res);

            // Aggregation errors add additional context to the error message, so we use a helper
            // function to validate the error message.
            mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
            let pipeline = [{'$listSearchIndexes': {'id': 'indexID'}}];
            let expectErrMsg = manageSearchIndexCommandResponse["errmsg"];
            let expectedCode = manageSearchIndexCommandResponse["code"];
            assertErrCodeAndErrMsgContains(testDB[collName], pipeline, expectedCode, expectErrMsg);

            // Exercise returning a IndexInformationTooLarge error that only the remote search index
            // management server generates.
            const manageSearchIndexListIndexesResponse = {
                ok: 0,
                errmsg: "the search index information for this collection exceeds 16 MB",
                code: 396,
                codeName: "IndexInformationTooLarge",
            };
            mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexListIndexesResponse);
            res = assert.commandFailedWithCode(testDB.runCommand({'listSearchIndexes': collName}),
                                               ErrorCodes.IndexInformationTooLarge);
            delete res.$clusterTime;
            delete res.operationTime;
            assert.eq(manageSearchIndexListIndexesResponse, res);

            // Test the $listSearchIndexes aggregation stage propagates the
            // 'IndexInformationTooLarge' error.
            mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexListIndexesResponse);
            pipeline = [{'$listSearchIndexes': {'id': 'indexID'}}];
            expectErrMsg = manageSearchIndexListIndexesResponse["errmsg"];
            expectedCode = manageSearchIndexListIndexesResponse["code"];
            assertErrCodeAndErrMsgContains(testDB[collName], pipeline, expectedCode, expectErrMsg);
        };
        runRemoteSearchIndexManagementServerErrorsTest(mongos);
        runRemoteSearchIndexManagementServerErrorsTest(st.shard0);
    }
}

testAgainstCollection(shardedCollName);
testAgainstCollection(unshardedCollName);

st.stop();
mongotMock.stop();
