/**
 * Control mongotmock.
 */

/**
 * Helper to create an expected command for mongot.
 *
 * @param {Object} query - The query to be recieved by mongot.
 * @param {String} collName - The collection name.
 * @param {String} db - The database name.
 * @param {BinaryType} collectionUUID - The binary representation of a collection's UUID.
 * @param {Int} protocolVersion - Optional: the version of mongot's merging logic.
 * @param {Object} cursorOptions - Optional: contains options for mongot to create the cursor such
 *     as number of requested docs.
 */
export function mongotCommandForQuery(
    query, collName, db, collectionUUID, protocolVersion = null, cursorOptions = null) {
    let cmd = {search: collName, $db: db, collectionUUID, query};
    if (protocolVersion != null) {
        cmd.intermediate = protocolVersion;
    }
    if (cursorOptions != null) {
        cmd.cursorOptions = cursorOptions;
    }
    return cmd;
}

/**
 * Helper to create an expected response from mongot with a batch of results.
 *
 * @param {Array} nextBatch - Array of documents to be returned in this response.
 * @param {Number} id - The mongot cursor ID.
 * @param {String} ns - The namespace of the collection our response is for.
 * @param {Boolean} ok - True when this response is not from an error.
 */
export function mongotResponseForBatch(nextBatch, id, ns, ok) {
    return {ok, cursor: {id, ns, nextBatch}};
}

/**
 * Same as above but for multiple cursors.
 */
export function mongotMultiCursorResponseForBatch(
    firstCursorBatch, firstId, secondCursorBatch, secondId, ns, ok) {
    return {
        ok,
        cursors: [
            {cursor: {id: firstId, ns, nextBatch: firstCursorBatch, type: "results"}, ok},
            {cursor: {id: secondId, ns, nextBatch: secondCursorBatch, type: "meta"}, ok}
        ]
    };
}

/**
 * Helper to create a killCursors response from mongotmock.
 * @param {String} collName - Name of collection.
 * @param {NumberLong} cursorId - The cursor which we expect mongod to kill.
 */
export function mongotKillCursorResponse(collName, cursorId) {
    return {
        expectedCommand: {killCursors: collName, cursors: [cursorId]},
        response: {
            cursorsKilled: [cursorId],
            cursorsNotFound: [],
            cursorsAlive: [],
            cursorsUnknown: [],
            ok: 1,
        }
    };
}

/**
 * Helper to mock the response of 'planShardedSearch' which don't care about the results of the
 * merging pipeline on any connection (shard or mongos).
 * @param {String} collName
 * @param {Object} query
 * @param {String} dbName
 * @param {Object} sortSpec
 * @param {ShardingTestWithMongotMock} stWithMock
 * @param {Mongo} conn
 */
export function mockPlanShardedSearchResponseOnConn(
    collName, query, dbName, sortSpec, stWithMock, conn, maybeUnused = false) {
    mockPlanShardedSearchResponse.cursorId++;
    let resp = {
        ok: 1,
        protocolVersion: NumberInt(1),
        // Tests calling this don't use metadata. Give a trivial pipeline.
        metaPipeline: [{$limit: 1}]
    };
    if (sortSpec != undefined) {
        resp["sortSpec"] = sortSpec;
    }
    const mergingPipelineHistory = [{
        expectedCommand: {
            planShardedSearch: collName,
            query: query,
            $db: dbName,
            searchFeatures: {shardedSort: 1}
        },
        response: resp,
        maybeUnused
    }];
    const mongot = stWithMock.getMockConnectedToHost(conn);
    let host = mongot.getConnection().host;
    mongot.setMockResponses(mergingPipelineHistory, mockPlanShardedSearchResponse.cursorId);
}

/**
 * Convenience helper function to simulate mockPlanShardedSearchResponseOnConn specifically on
 * mongos, which is the most common usage.
 */
export function mockPlanShardedSearchResponse(collName, query, dbName, sortSpec, stWithMock) {
    mockPlanShardedSearchResponseOnConn(
        collName, query, dbName, sortSpec, stWithMock, stWithMock.st.s);
}

mockPlanShardedSearchResponse.cursorId = 1423;

export function mongotCommandForVectorSearchQuery({
    queryVector,
    path,
    numCandidates = null,
    limit,
    index = null,
    filter = null,
    explain = null,
    collName,
    dbName,
    collectionUUID
}) {
    assert.eq(arguments.length, 1, "Expected one argument to mongotCommandForVectorSearchQuery()");
    let cmd = {
        vectorSearch: collName,
        $db: dbName,
        collectionUUID,
        queryVector,
        path,
        limit,
    };

    if (numCandidates) {
        cmd.numCandidates = numCandidates;
    }

    if (index) {
        cmd.index = index;
    }

    if (filter) {
        cmd.filter = filter;
    }

    if (explain) {
        cmd.explain = explain;
    }

    return cmd;
}

export class MongotMock {
    /**
     * Create a new mongotmock.
     */
    constructor(options) {
        this.mongotMock = "mongotmock";
        this.pid = undefined;
        this.port = allocatePort();
        this.conn = undefined;
        this.dataDir = (options && options.dataDir) || MongoRunner.dataDir + "/mongotmock";
        if (!pathExists(this.dataDir)) {
            resetDbpath(this.dataDir);
        }
        this.dataDir = this.dataDir + "/" + this.port;
        resetDbpath(this.dataDir);
    }

    /**
     *  Start mongotmock and wait for it to start.
     */
    start(opts = {bypassAuth: false}) {
        print("mongotmock: " + this.port);

        const conn_str = this.dataDir + "/mongocryptd.sock";
        const args = [this.mongotMock];

        args.push("--port=" + this.port);
        // mongotmock uses mongocryptd.sock.
        args.push("--unixSocketPrefix=" + this.dataDir);

        args.push("--setParameter");
        args.push("enableTestCommands=1");
        args.push("-vvv");

        args.push("--pidfilepath=" + this.dataDir + "/cryptd.pid");

        if (TestData && TestData.auth && !opts.bypassAuth) {
            args.push("--clusterAuthMode=keyFile");
            args.push("--keyFile=" + TestData.keyFile);
        }

        this.pid = _startMongoProgram({args: args});

        assert(checkProgram(this.pid));

        // Wait for connection to be established with server
        var conn = null;
        const pid = this.pid;
        const port = this.port;

        assert.soon(function() {
            try {
                conn = new Mongo(conn_str);
                if (TestData && TestData.auth && opts.bypassAuth) {
                    // if Mongot is opting out of auth, we don't need to
                    // authenticate our connection to it.
                    conn.authenticated = true;
                }
                conn.pid = pid;
                return true;
            } catch (e) {
                var res = checkProgram(pid);
                if (!res.alive) {
                    print("Could not start mongo program at " + conn_str +
                          ", process ended with exit code: " + res.exitCode);
                    return true;
                }
            }
            return false;
        }, "unable to connect to mongo program on port " + conn_str, 30 * 1000);

        this.conn = conn;
        print("mongotmock sucessfully started.");
    }

    /**
     *  Stop mongotmock, asserting that it shutdown cleanly.
     */
    stop() {
        // Check the remaining history on the mock. There should be 0 remaining queued commands.
        this.assertEmpty();

        return stopMongoProgramByPid(this.pid);
    }

    /**
     * Returns a connection to mongotmock.
     */
    getConnection() {
        return this.conn;
    }

    /**
     * Convenience function to set expected commands and responses for the mock mongot.
     *
     * @param {Array} expectedMongotMockCmdsAndResponses - Array of [expectedCommand, response]
     * pairs for the mock mongot.
     * @param {Number} cursorId - The mongot cursor ID.
     * @param {Number} additionalCursorId - If the initial command will return multiple cursors, and
     *     there is no getMore to set the response for the state, pass this instead.
     */
    setMockResponses(expectedMongotMockCmdsAndResponses, cursorId, additionalCursorId = 0) {
        const connection = this.getConnection();
        const setMockResponsesCommand = {
            setMockResponses: 1,
            cursorId: NumberLong(cursorId),
            history: expectedMongotMockCmdsAndResponses,
        };
        assert.commandWorked(connection.getDB("mongotmock").runCommand(setMockResponsesCommand));
        if (additionalCursorId !== 0) {
            assert.commandWorked(connection.getDB("mongotmock").runCommand({
                allowMultiCursorResponse: 1,
                cursorId: NumberLong(additionalCursorId)
            }));
        }
    }

    /**
     * Set whether or not to check if commands arrived in the expected order. Should only be
     * disabled for tests with non-deterministic behavior.
     */
    setOrderCheck(boolVal) {
        assert.commandWorked(this.getConnection().adminCommand({"setOrderCheck": boolVal}));
    }
    disableOrderCheck() {
        this.setOrderCheck(false);
    }

    /**
     * Verify that no responses remain enqueued in the mock. Call this in between consecutive tests.
     */
    assertEmpty() {
        const connection = this.getConnection();
        const resp = assert.commandWorked(connection.adminCommand({getQueuedResponses: 1}));

        // Assert that either there are either
        // 1. No remaining responses
        // 2. All remaining responses are marked with 'maybeUnused'
        if (resp.numRemainingResponses == 0) {
            return;
        }
        for (const cursorId in resp) {
            let mockResponses = resp[cursorId];

            if (!cursorId.startsWith("cursorID")) {
                continue;
            }
            // Iterate over all responses queued and assert that they must have 'maybeUnused' set to
            // true.
            for (const key in mockResponses) {
                let r = mockResponses[key];

                assert(r.hasOwnProperty("maybeUnused") && r["maybeUnused"],
                       `found unused response for ${cursorId}: ${tojson(r)}`);
            }
        }
    }

    /**
     * Sets the manageSearchIndex mongot mock command response to return
     * 'expectedManageSearchIndexResponse' to a single caller.
     */
    setMockSearchIndexCommandResponse(expectedManageSearchIndexResponse) {
        const connection = this.getConnection();
        const setManageSearchIndexResponseCommand = {
            setManageSearchIndexResponse: 1,
            manageSearchIndexResponse: expectedManageSearchIndexResponse
        };
        assert.commandWorked(
            connection.getDB('mongotmock').runCommand(setManageSearchIndexResponseCommand));
    }

    /**
     * Calls the manageSearchIndex mongot mock command to get the mock response.
     */
    callManageSearchIndexCommand() {
        const connection = this.getConnection();
        const manageSearchIndexCommand = {
            manageSearchIndex: "fake-coll-name",
            collectionUUID: UUID(),
            userCommand: {"user-command-field": "user-command-value"},
        };
        return assert.commandWorked(
            connection.getDB('mongotmock').runCommand(manageSearchIndexCommand));
    }

    /**
     * Convenience function that instructs the mongotmock to close the incoming connection
     * on which it receives a command in response to the next `n` search commands.
     */
    closeConnectionInResponseToNextNRequests(n) {
        const connection = this.getConnection();
        assert.commandWorked(
            connection.adminCommand({closeConnectionOnNextRequests: NumberInt(n)}));
    }
}
