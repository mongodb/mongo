/**
 * Test mongotmock.
 */
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

const mongotMock = new MongotMock();
mongotMock.start();

const conn = mongotMock.getConnection();
const testDB = conn.getDB("test");

function ensureNoResponses() {
    const resp = assert.commandWorked(testDB.runCommand({getQueuedResponses: 1}));
    assert.eq(resp.numRemainingResponses, 0);
}

{
    // Ensure the mock returns the correct responses and validates the 'expected' commands.
    // These examples do not obey the find/getMore protocol.
    const cursorId = NumberLong(123);
    const searchCmd = {search: "a UUID"};
    const history = [
        {expectedCommand: searchCmd, response: {ok: 1, foo: 1}},
        {expectedCommand: {getMore: cursorId, collection: "abc"}, response: {ok: 1, foo: 2}},
        {expectedCommand: {getMore: cursorId, collection: "abc"}, response: {ok: 1, foo: 3}},
    ];

    assert.commandWorked(
        testDB.runCommand({setMockResponses: 1, cursorId: cursorId, history: history}));

    // Now run a search command.
    let resp = assert.commandWorked(testDB.runCommand(searchCmd));
    assert.eq(resp, {ok: 1, foo: 1});

    // Run a getMore which succeeds.
    resp = assert.commandWorked(testDB.runCommand({getMore: NumberLong(123), collection: "abc"}));
    assert.eq(resp, {ok: 1, foo: 2});

    // Check the remaining history on the mock. There should be one more queued command.
    resp = assert.commandWorked(testDB.runCommand({getQueuedResponses: 1}));
    assert.eq(resp.numRemainingResponses, 1);

    // Run another getMore which should succeed.
    resp = assert.commandWorked(testDB.runCommand({getMore: NumberLong(123), collection: "abc"}));
    assert.eq(resp, {ok: 1, foo: 3});

    // Run another getMore. This should fail because there are no more queued responses for the
    // cursor id.
    assert.commandFailedWithCode(testDB.runCommand({getMore: NumberLong(123), collection: "abc"}),
                                 31087);

    // Check the remaining history on the mock. There should be 0 remaining queued commands.
    ensureNoResponses();
}

{
    // Test some edge and error cases.
    const cursorId = NumberLong(123);
    const searchCmd = {search: "a UUID"};
    const history = [
        {expectedCommand: searchCmd, response: {ok: 1}},
    ];

    assert.commandWorked(
        testDB.runCommand({setMockResponses: 1, cursorId: cursorId, history: history}));

    // We should be able to set the mock responses again to the same thing without issue.
    assert.commandWorked(
        testDB.runCommand({setMockResponses: 1, cursorId: cursorId, history: history}));

    // Run setMockResponses on cursor id of 0.
    assert.commandFailedWithCode(
        testDB.runCommand({setMockResponses: 1, cursorId: NumberLong(0), history: history}),
        ErrorCodes.InvalidOptions);

    // Run getMore on cursor id before it's ready.
    assert.commandFailedWithCode(testDB.runCommand({getMore: NumberLong(123), collection: "abc"}),
                                 31088);

    // Run getMore on invalid cursor id.
    assert.commandFailedWithCode(testDB.runCommand({getMore: NumberLong(777), collection: "abc"}),
                                 31089);

    // Run a search which doesn't match its 'expectedCommand'.
    assert.commandFailedWithCode(testDB.runCommand({search: "a different UUID"}), 31086);

    // Reset state associated with cursor id and run a search command which doesn't match its
    // 'expectedCommand' by having extra fields compared to the expected.
    assert.commandWorked(
        testDB.runCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
    assert.commandFailedWithCode(testDB.runCommand({search: "a UUID", random: "yes"}), 31086);

    // Reset state associated with cursor id and run a search command which should fail since it has
    // a field that should not be ignored in a subobject.
    assert.commandWorked(
        testDB.runCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
    assert.commandFailedWithCode(
        testDB.runCommand({search: "a UUID", cursorOptions: {notInIgnoredFieldSet: 1}}), 31086);

    // Reset state associated with cursor id and run a search command which should succeed since it
    // has fields that should be ignored.
    assert.commandWorked(
        testDB.runCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
    assert.commandWorked(
        testDB.runCommand({search: "a UUID", cursorOptions: {docsRequested: 1, batchSize: 1}}));

    // Reset the state associated with the cursor id and run a search command which
    // succeeds.
    assert.commandWorked(
        testDB.runCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
    assert.commandWorked(testDB.runCommand({search: "a UUID"}));
    // Run another search command. We did not set up any state on the mock for another
    // client, though, so this should fail.
    assert.commandFailedWithCode(testDB.runCommand({search: "a UUID"}), 31094);
}

//
// The client in the remaining tests is well-behaving and obeys the find/getMore cursor
// iteration protocol.
//

// Open a cursor and exhaust it.
{
    const cursorId = NumberLong(123);
    const searchCmd = {search: "a UUID"};
    const cursorHistory = [
        {
            expectedCommand: searchCmd,
            response:
                {ok: 1, cursor: {firstBatch: [{_id: 0}, {_id: 1}], id: cursorId, ns: "testColl"}}
        },
        {
            expectedCommand: {getMore: cursorId, collection: "testColl"},
            response: {
                ok: 1,
                cursor: {
                    id: cursorId,
                    ns: "testColl",
                    nextBatch: [
                        {_id: 2},
                        {_id: 3},
                    ]
                }
            }
        },
        {
            expectedCommand: {getMore: cursorId, collection: "testColl"},
            response: {
                ok: 1,
                cursor: {
                    id: NumberLong(0),
                    ns: "testColl",
                    nextBatch: [
                        {_id: 4},
                    ]
                }
            }
        },
    ];

    assert.commandWorked(
        testDB.runCommand({setMockResponses: 1, cursorId: cursorId, history: cursorHistory}));
    let resp = assert.commandWorked(testDB.runCommand(searchCmd));

    const cursor = new DBCommandCursor(testDB, resp);
    const arr = cursor.toArray();
    assert.eq(arr, [{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}]);

    // Make sure there are no remaining queued responses.
    ensureNoResponses();
}

// Open a cursor, but don't exhaust it, checking the 'killCursors' functionality of mongotmock.
{
    const cursorId = NumberLong(123);
    const searchCmd = {search: "a UUID"};
    const cursorHistory = [
        {
            expectedCommand: searchCmd,
            response:
                {ok: 1, cursor: {firstBatch: [{_id: 0}, {_id: 1}], id: cursorId, ns: "testColl"}}
        },
        {
            expectedCommand: {killCursors: "testColl", cursors: [cursorId]},
            response: {
                cursorsKilled: [cursorId],
                cursorsNotFound: [],
                cursorsAlive: [],
                cursorsUnknown: [],
                ok: 1,
            }
        },
    ];

    assert.commandWorked(
        testDB.runCommand({setMockResponses: 1, cursorId: cursorId, history: cursorHistory}));

    let resp = assert.commandWorked(testDB.runCommand(searchCmd));

    {
        const cursor = new DBCommandCursor(testDB, resp);

        const next = cursor.next();
        assert.eq(next, {_id: 0});

        // Don't iterate the cursor any more! We want to make sure the DBCommandCursor has to
        // kill it.
        cursor.close();
    }

    // Make sure there are no remaining queued responses.
    ensureNoResponses();
}

// Open a cursor, exhaust it, and then explicitly send a killCursors command.
{
    const cursorId = NumberLong(123);
    const searchCmd = {search: "a UUID"};
    const cursorHistory = [{
        expectedCommand: searchCmd,
        response: {
            ok: 1,
            cursor: {
                firstBatch: [{_id: 0}],
                id: 0,  // cursorID of 0 in response indicates end of stream
                ns: "testColl"
            }
        }
    }];

    assert.commandWorked(
        testDB.runCommand({setMockResponses: 1, cursorId: cursorId, history: cursorHistory}));

    let resp = assert.commandWorked(testDB.runCommand(searchCmd));

    {
        const cursor = new DBCommandCursor(testDB, resp);

        const next = cursor.next();
        assert.eq(next, {_id: 0});
        assert(cursor.isExhausted());

        assert.commandWorked(testDB.runCommand({killCursors: "testColl", cursors: [cursorId]}));
    }
}

// Test with multiple clients.
{
    const searchCmd = {search: "a UUID"};

    const cursorIdA = NumberLong(123);
    const cursorAHistory = [
        {
            expectedCommand: searchCmd,
            response: {
                ok: 1,
                cursor: {firstBatch: [{_id: "cursor A"}, {_id: 1}], id: cursorIdA, ns: "testColl"}
            }
        },
        {
            expectedCommand: {getMore: cursorIdA, collection: "testColl"},
            response: {
                ok: 1,
                cursor: {
                    id: NumberLong(0),
                    ns: "testColl",
                    nextBatch: [
                        {_id: 2},
                        {_id: 3},
                    ]
                }
            }
        },
    ];

    const cursorIdB = NumberLong(456);
    const cursorBHistory = [
        {
            expectedCommand: searchCmd,
            response: {
                ok: 1,
                cursor: {firstBatch: [{_id: "cursor B"}, {_id: 1}], id: cursorIdB, ns: "testColl"}
            }
        },
        {
            expectedCommand: {getMore: cursorIdB, collection: "testColl"},
            response: {
                ok: 1,
                cursor: {
                    id: NumberLong(0),
                    ns: "testColl",
                    nextBatch: [
                        {_id: 2},
                        {_id: 3},
                    ]
                }
            }
        },
    ];

    assert.commandWorked(
        testDB.runCommand({setMockResponses: 1, cursorId: cursorIdA, history: cursorAHistory}));
    assert.commandWorked(
        testDB.runCommand({setMockResponses: 1, cursorId: cursorIdB, history: cursorBHistory}));

    let responses = [
        assert.commandWorked(testDB.runCommand(searchCmd)),
        assert.commandWorked(testDB.runCommand(searchCmd))
    ];

    const cursors =
        [new DBCommandCursor(testDB, responses[0]), new DBCommandCursor(testDB, responses[1])];

    // The mock responses should respect a FIFO order. The first cursor should get the
    // responses for cursor A, and the second cursor should get the responses for cursor B.
    {
        const firstDoc = cursors[0].next();
        assert.eq(firstDoc._id, "cursor A");
    }

    {
        const firstDoc = cursors[1].next();
        assert.eq(firstDoc._id, "cursor B");
    }

    // Iterate the two cursors together.
    const nDocsPerCursor = 4;
    for (let i = 1; i < nDocsPerCursor; i++) {
        for (let c of cursors) {
            const doc = c.next();
            assert.eq(doc._id, i);
        }
    }

    // Make sure there are no remaining queued responses.
    ensureNoResponses();
}

// Test that mongotmock can return a merging pipeline.
{
    const cursorId = NumberLong(123);
    const pipelineCmd = {
        "planShardedSearch": "collName",
        "db": testDB.getName(),
        "collectionUUID": "522cdf5e-54fc-4230-9d45-49da990e8ea7",
        "query": {"text": {"path": "title", "query": "godfather"}},
        "searchFeatures": {"shardedSort": 1}
    };
    const cursorHistory = [
        {
            expectedCommand: pipelineCmd,
            // Real metaPipelines will be significantly larger, but this is fine for testing the
            // mock.
            response: {
                ok: 1,
                protocolVersion: 1,
                metaPipeline: [{$documents: [{facetOne: 1, facetTwo: 2}]}]
            }
        },
    ];

    assert.commandWorked(
        testDB.runCommand({setMockResponses: 1, cursorId: cursorId, history: cursorHistory}));
    let resp = assert.commandWorked(testDB.runCommand(pipelineCmd));
    assert.eq(resp, cursorHistory[0].response);

    // Make sure there are no remaining queued responses.
    ensureNoResponses();
}

// Test that mongotmock can return a vectorSearch request.
{
    const cursorId = NumberLong(123);
    const resultsABatch1 = [
        {_id: 1, $vectorSearchScore: .4},
        {_id: 2, $vectorSearchScore: .3},
    ];
    const resultsABatch2 = [{_id: 3, $vectorSearchScore: 0.123}];
    const vectorSearchCmd = {
        "vectorSearch": "collName",
        "db": testDB.getName(),
        "collectionUUID": "522cdf5e-54fc-4230-9d45-49da990e8ea7",
        "queryVector": [1.1, 2.2, 3.3],
        "path": "indexedField1",
        "index": "vectorIndex1",
        "numCandidates": 100,
        "limit": 10,
    };
    const cursorHistory = [
        {
            expectedCommand: vectorSearchCmd,
            response: {
                ok: 1,
                cursor: {id: cursorId, ns: "testColl"},
                type: "results",
                nextBatch: resultsABatch1,
            }
        },

        // GetMore for results cursor
        {
            expectedCommand: {getMore: cursorId, collection: "testColl"},
            response: {
                ok: 1,
                cursor: {id: cursorId, ns: "testColl"},
                type: "results",
                nextBatch: resultsABatch2,
            }
        },
    ];

    assert.commandWorked(
        testDB.runCommand({setMockResponses: 1, cursorId: cursorId, history: cursorHistory}));
    let resp = assert.commandWorked(testDB.runCommand(vectorSearchCmd));
    assert.eq(resp, cursorHistory[0].response);

    resp = assert.commandWorked(testDB.runCommand({getMore: cursorId, collection: "testColl"}));
    assert.eq(resp, cursorHistory[1].response);

    // Make sure there are no remaining queued responses.
    ensureNoResponses();
}

// Test that mongotmock can process commands out of order when 'checkOrder' is disabled.
{
    const resultDocCommand = {search: "searchQuery"};
    const planShardedSearchCommand = {
        planShardedSearch: "collName",
        query: resultDocCommand,
    };
    const expectedDocResults = [{_id: 0}, {_id: 1}];
    const expectedPssResult = {ok: 1, protocolVersion: NumberInt(1), metaPipeline: [{"$limit": 1}]};
    // Setup a mock with a search command and a planShardedSearch command.
    function setHistoryForOrderTest() {
        const planShardedSearchHistory = [{
            expectedCommand: planShardedSearchCommand,
            response: expectedPssResult,
        }];
        const resultDocHistory = [
            {
                expectedCommand: resultDocCommand,
                response: {
                    ok: 1,
                    cursor: {firstBatch: expectedDocResults, id: NumberLong(0), ns: "testColl"}
                }
            },
        ];
        assert.commandWorked(testDB.runCommand(
            {setMockResponses: 1, cursorId: NumberLong(1), history: planShardedSearchHistory}));
        assert.commandWorked(testDB.runCommand(
            {setMockResponses: 1, cursorId: NumberLong(2), history: resultDocHistory}));
    }
    function assertResults(actualDocResults, actualPssResults) {
        assert.eq(actualDocResults, expectedDocResults);
        assert.eq(actualPssResults, expectedPssResult);
    }
    setHistoryForOrderTest();

    // Make sure we fail if we call the commands out of order.
    assert.commandFailedWithCode(testDB.runCommand(resultDocCommand), 31086);

    // Reset the mock.
    assert.commandWorked(testDB.runCommand({"clearQueuedResponses": {}}));
    ensureNoResponses();
    // Disable order checking.
    assert.commandWorked(testDB.runCommand({setOrderCheck: false}));

    // Repeat the same setup.
    setHistoryForOrderTest();
    // Call the commands out of order.
    let docResults = assert.commandWorked(testDB.runCommand(resultDocCommand));
    let pssResult = assert.commandWorked(testDB.runCommand(planShardedSearchCommand));
    assertResults(docResults.cursor.firstBatch, pssResult);
    ensureNoResponses();

    // Repeat the same setup.
    setHistoryForOrderTest();

    // Call the commands in order.
    assert.commandWorked(testDB.runCommand(planShardedSearchCommand));
    assert.commandWorked(testDB.runCommand(resultDocCommand));
    assertResults(docResults.cursor.firstBatch, pssResult);

    // Enable order checking for future tests.
    assert.commandWorked(testDB.runCommand({setOrderCheck: true}));

    // Make sure there are no remaining queued responses.
    ensureNoResponses();
}

// Test that mongotmock can process multi cursor commands out of order when 'checkOrder' is
// disabled.
{
    const multiCursorCommandA = {
        search: "multiCursorAQuery",
    };
    const multiCursorCommandB = {search: "multiCursorBQuery"};
    const resultsABatch1 = [
        {_id: 1, val: 1, $searchScore: .4},
        {_id: 2, val: 2, $searchScore: .3},
    ];
    const resultsABatch2 = [{_id: 3, val: 3, $searchScore: 0.123}];
    const resultsAMetaResult = [{metaVal: 1}, {metaVal: 2}];
    const resultsBBatch1 = [
        {_id: 1, val: 1, $searchScore: .4},
        {_id: 2, val: 2, $searchScore: .3},
    ];
    const resultsBBatch2 = [{_id: 3, val: 3, $searchScore: 0.123}];
    const resultsBMetaResult = [{metaVal: 1}, {metaVal: 2}];
    // Setup a mock with a document command and a planShardedSearch command.
    function setHistoryForOrderTest() {
        const historyA = [
            {
                expectedCommand: multiCursorCommandA,
                response: {
                    ok: 1,
                    cursors: [
                        {
                            cursor: {
                                id: NumberLong(1),
                                type: "results",
                                ns: "collName",
                                nextBatch: resultsABatch1,
                            },
                            ok: 1
                        },
                        {
                            cursor: {
                                id: NumberLong(10),
                                ns: "collName",
                                type: "meta",
                                nextBatch: resultsAMetaResult,
                            },
                            ok: 1
                        }
                    ]
                }
            },
            // GetMore for results cursor
            {
                expectedCommand: {getMore: NumberLong(1), collection: "collName"},
                response:
                    {cursor: {id: NumberLong(0), ns: "collName", nextBatch: resultsABatch2}, ok: 1}
            },
        ];
        const historyB = [
            {
                expectedCommand: multiCursorCommandB,
                response: {
                    ok: 1,
                    cursors: [
                        {
                            cursor: {
                                id: NumberLong(2),
                                type: "results",
                                ns: "collName",
                                nextBatch: resultsBBatch1,
                            },
                            ok: 1
                        },
                        {
                            cursor: {
                                id: NumberLong(20),
                                ns: "collName",
                                type: "meta",
                                nextBatch: resultsBMetaResult,
                            },
                            ok: 1
                        }
                    ]
                }
            },
            // GetMore for results cursor
            {
                expectedCommand: {getMore: NumberLong(2), collection: "collName"},
                response: {
                    cursor: {
                        id: NumberLong(0),
                        ns: "collName",
                        nextBatch: resultsBBatch2,
                    },
                    ok: 1
                }
            },
        ];
        assert.commandWorked(
            testDB.runCommand({setMockResponses: 1, cursorId: NumberLong(1), history: historyA}));
        assert.commandWorked(
            testDB.runCommand({allowMultiCursorResponse: 1, cursorId: NumberLong(10)}));
        assert.commandWorked(
            testDB.runCommand({setMockResponses: 1, cursorId: NumberLong(2), history: historyB}));
        assert.commandWorked(
            testDB.runCommand({allowMultiCursorResponse: 1, cursorId: NumberLong(20)}));
    }

    // Return the batches from both cursors in the response. Assumes the format of the response from
    // this test.
    function getBatchesFromResponse(response) {
        const resultDocs = response.cursors[0].cursor.nextBatch;
        const resultMeta = response.cursors[1].cursor.nextBatch;
        return [resultDocs, resultMeta];
    }

    function assertResultsFromAllCursors(resultsA, getMoreA, resultsB, getMoreB) {
        const resultACursors = getBatchesFromResponse(resultsA);
        const resultBCursors = getBatchesFromResponse(resultsB);
        assert.eq(resultACursors[0], resultsABatch1);
        assert.eq(resultACursors[1], resultsAMetaResult);
        assert.eq(getMoreA.cursor.nextBatch, resultsABatch2);
        assert.eq(resultBCursors[0], resultsBBatch1);
        assert.eq(resultBCursors[1], resultsBMetaResult);
        assert.eq(getMoreB.cursor.nextBatch, resultsBBatch2);
    }
    setHistoryForOrderTest();

    // Make sure we fail if we call the commands out of order.
    assert.commandFailedWithCode(testDB.runCommand(multiCursorCommandB), 31086);

    // Reset the mock.
    assert.commandWorked(testDB.runCommand({"clearQueuedResponses": {}}));
    ensureNoResponses();
    // Disable order checking.
    assert.commandWorked(testDB.runCommand({setOrderCheck: false}));

    // Repeat the same setup.
    setHistoryForOrderTest();

    // Call the commands out of order.
    let resultsB = assert.commandWorked(testDB.runCommand(multiCursorCommandB));
    let resultsA = assert.commandWorked(testDB.runCommand(multiCursorCommandA));
    let getMoreA =
        assert.commandWorked(testDB.runCommand({getMore: NumberLong(1), collection: "collName"}));
    let getMoreB =
        assert.commandWorked(testDB.runCommand({getMore: NumberLong(2), collection: "collName"}));
    assertResultsFromAllCursors(resultsA, getMoreA, resultsB, getMoreB);
    ensureNoResponses();

    // Demonstrate that getMore cannot be run before the originating command even with order
    // checking disabled.
    setHistoryForOrderTest();
    assert.commandFailedWithCode(
        testDB.runCommand({getMore: NumberLong(1), collection: "collName"}), 31088);
    assert.commandFailedWithCode(
        testDB.runCommand({getMore: NumberLong(2), collection: "collName"}), 31088);
    assert.commandWorked(testDB.runCommand(multiCursorCommandB));
    assert.commandWorked(testDB.runCommand(multiCursorCommandA));
    assert.commandWorked(testDB.runCommand({getMore: NumberLong(2), collection: "collName"}));
    assert.commandWorked(testDB.runCommand({getMore: NumberLong(1), collection: "collName"}));
    ensureNoResponses();

    // Repeat the same setup.
    setHistoryForOrderTest();

    // Call the commands in order.
    resultsB = assert.commandWorked(testDB.runCommand(multiCursorCommandB));
    resultsA = assert.commandWorked(testDB.runCommand(multiCursorCommandA));
    getMoreB =
        assert.commandWorked(testDB.runCommand({getMore: NumberLong(2), collection: "collName"}));
    getMoreA =
        assert.commandWorked(testDB.runCommand({getMore: NumberLong(1), collection: "collName"}));
    assertResultsFromAllCursors(resultsA, getMoreA, resultsB, getMoreB);

    // Enable order checking for future tests.
    assert.commandWorked(testDB.runCommand({setOrderCheck: true}));

    // Make sure there are no remaining queued responses.
    ensureNoResponses();
}

// Ensure that if the mock has unused responses, we error.
{
    const cursorId = NumberLong(123);
    const searchCmd = {search: "a UUID"};

    // Set up the mock's history with responses with maybeUnused as explicitly false.
    const history = [
        {expectedCommand: searchCmd, response: {ok: 1, foo: 1}, maybeUnused: false},
        {
            expectedCommand: {getMore: cursorId, collection: "abc"},
            response: {ok: 1, foo: 2},
            maybeUnused: false
        },
    ];

    assert.commandWorked(
        testDB.runCommand({setMockResponses: 1, cursorId: cursorId, history: history}));

    // Now run a search command.
    let resp = assert.commandWorked(testDB.runCommand(searchCmd));
    assert.eq(resp, {ok: 1, foo: 1});

    // Assert that assertEmpty fails at this point.
    const errMsg = assert.throws(() => mongotMock.assertEmpty());
    assert.neq(-1, errMsg.message.indexOf("found unused response for cursorID 123"));

    // Run a getMore which succeeds to exhaust the history.
    resp = assert.commandWorked(testDB.runCommand({getMore: NumberLong(123), collection: "abc"}));
    assert.eq(resp, {ok: 1, foo: 2});

    // Check that assertEmpty now succeeds.
    mongotMock.assertEmpty();
}

mongotMock.stop();
