/**
 * Control mongotmock.
 * @tags: [
 *   # Unix domain sockets are not available on windows.
 *   incompatible_with_windows_tls
 * ]
 */

import {CA_CERT, CLIENT_CERT, SERVER_CERT} from "jstests/ssl/libs/ssl_helpers.js";
import {getDefaultExplainContents, getDefaultLastExplainContents} from "jstests/with_mongot/mongotmock/lib/utils.js";

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
 * @param {Object} explainVerbosity - Optional: contains explain verbosity object, i.e. {verbosity:
 *     "queryPlanner"}
 * @param {Object} optimizationFlags - Optional: contains optimization options for mongot
 */
export function mongotCommandForQuery({
    query,
    collName,
    db,
    collectionUUID,
    protocolVersion = null,
    cursorOptions = null,
    explainVerbosity = null,
    viewName = null,
    optimizationFlags = null,
}) {
    let cmd = {search: collName, $db: db, collectionUUID, query};
    if (protocolVersion != null) {
        cmd.intermediate = protocolVersion;
    }
    if (cursorOptions != null) {
        cmd.cursorOptions = cursorOptions;
    }
    if (explainVerbosity != null) {
        cmd.explain = explainVerbosity;
    }
    if (viewName != null) {
        cmd.viewName = viewName;
    }
    if (optimizationFlags != null) {
        cmd.optimizationFlags = optimizationFlags;
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
 * @param {Object} vars - Object containing var information
 * @param {Object} explainContents - Object containing explain information.
 */
export function mongotResponseForBatch(nextBatch, id, ns, ok, explainContents = null, vars = null) {
    let mongotResponse = {ok, cursor: {id, ns, nextBatch}};
    if (explainContents != null) {
        mongotResponse.explain = explainContents;
    }
    if (vars != null) {
        mongotResponse.vars = vars;
    }
    return mongotResponse;
}

/**
 * Same as above but for multiple cursors.
 */
export function mongotMultiCursorResponseForBatch(
    firstCursorBatch,
    firstId,
    secondCursorBatch,
    secondId,
    ns,
    ok,
    explainContents = null,
    metaExplainContents = null,
) {
    let resultsCursor = {
        cursor: {id: firstId, ns, nextBatch: firstCursorBatch, type: "results"},
        ok,
    };
    let metaCursor = {cursor: {id: secondId, ns, nextBatch: secondCursorBatch, type: "meta"}, ok};
    if (explainContents != null) {
        resultsCursor.explain = explainContents;
        assert(metaExplainContents, "metaExplainContents should not be null as explainContents is not null");
    }
    if (metaExplainContents != null) {
        metaCursor.explain = metaExplainContents;
        assert(explainContents, "explainContents should not be null as metaExplainContents is not null");
    }
    return {ok, cursors: [resultsCursor, metaCursor]};
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
        },
    };
}

const protocolVersion = NumberInt(1);

/**
 * Returns the protocolVersion used in mockPlanShardedSearchResponseOnConn() as it's necessary for
 * creating expected commands.
 */
export function getDefaultProtocolVersionForPlanShardedSearch() {
    return protocolVersion;
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
 * @param {bool} maybeUnused
 * @param {Object} explainVerbosity should be of the form {explain: explainVerbosity}
 */
export function mockPlanShardedSearchResponseOnConn(
    collName,
    query,
    dbName,
    sortSpec,
    stWithMock,
    conn,
    maybeUnused = false,
    explainVerbosity = null,
    hasSearchMetaStage = false,
) {
    mockPlanShardedSearchResponse.cursorId++;
    let resp = {
        ok: 1,
        protocolVersion: protocolVersion,
        // Tests calling this don't use metadata. Give a trivial pipeline.
        metaPipeline: [{$limit: 1}],
    };
    if (sortSpec != undefined) {
        resp["sortSpec"] = sortSpec;
    }

    let expectedCommand = {planShardedSearch: collName, query: query, $db: dbName, searchFeatures: {shardedSort: 1}};

    if (hasSearchMetaStage) {
        expectedCommand.optimizationFlags = {omitSearchDocumentResults: true};
    }

    if (explainVerbosity != null) {
        expectedCommand.explain = explainVerbosity;
    }
    const mergingPipelineHistory = [{expectedCommand, response: resp, maybeUnused}];
    const mongot = stWithMock.getMockConnectedToHost(conn);
    let host = mongot.getConnection().host;
    mongot.setMockResponses(mergingPipelineHistory, mockPlanShardedSearchResponse.cursorId);
}

/**
 * Convenience helper function to simulate mockPlanShardedSearchResponseOnConn specifically on
 * mongos, which is the most common usage.
 */
export function mockPlanShardedSearchResponse(
    collName,
    query,
    dbName,
    sortSpec,
    stWithMock,
    maybeUnused = false,
    explainVerbosity = null,
    hasSearchMetaStage = false,
) {
    mockPlanShardedSearchResponseOnConn(
        collName,
        query,
        dbName,
        sortSpec,
        stWithMock,
        stWithMock.st.s,
        maybeUnused,
        explainVerbosity,
        hasSearchMetaStage,
    );
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
    returnStoredSource = null,
    collName,
    dbName,
    collectionUUID,
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

    if (returnStoredSource !== null) {
        cmd.returnStoredSource = returnStoredSource;
    }

    return cmd;
}

export function mockAllRequestsWithBatchSizes({
    query,
    collName,
    dbName,
    collectionUUID,
    documents,
    expectedBatchSizes,
    cursorId,
    mongotMockConn,
    explainVerbosity = null,
}) {
    const responseOk = 1;

    // Tracks which doc to return as the start of the next batch.
    let docIndex = 0;
    const history = [];

    // Fill the expected mongot history based on the array of expectedBatchSizes.
    for (let i = 0; i < expectedBatchSizes.length; i++) {
        const batchSize = expectedBatchSizes[i];
        let expectedCommand;
        let response;

        // The first batch will be requested via a normal mongot request; the rest of the batches
        // will be requested via getMores.
        if (i === 0) {
            expectedCommand = mongotCommandForQuery({
                query,
                collName,
                db: dbName,
                collectionUUID,
                cursorOptions: {batchSize},
                explainVerbosity,
            });
        } else {
            expectedCommand = {getMore: cursorId, collection: collName, cursorOptions: {batchSize}};
        }

        // If this batch exhausts the remaining mongot results, return a response with cursorId = 0
        // to indicate the results have been fully exhausted. Otherwise, return the cursorId.
        if (docIndex + batchSize > documents.length) {
            response = mongotResponseForBatch(
                documents.slice(docIndex),
                NumberLong(0),
                dbName + "." + collName,
                responseOk,
                explainVerbosity ? getDefaultLastExplainContents() : null,
            );
        } else {
            response = mongotResponseForBatch(
                documents.slice(docIndex, docIndex + batchSize),
                cursorId,
                dbName + "." + collName,
                responseOk,
                explainVerbosity ? getDefaultExplainContents() : null,
            );
            docIndex += batchSize;
        }

        history.push({expectedCommand, response});
    }

    mongotMockConn.setMockResponses(history, cursorId);
}

const tlsModeOptions = ["disabled", "allowTLS", "preferTLS", "requireTLS"];
const kSIGTERM = 15;

export class MongotMock {
    /**
     * Create a new mongotmock.
     */
    constructor(options) {
        this.mongotMock = "mongotmock";
        this.pid = undefined;
        this.port = allocatePort();
        if (this.useGRPC()) {
            this.gRPCPort = allocatePort();
        }
        this.conn = undefined;
        this.dataDir = (options && options.dataDir) || MongoRunner.dataDir + "/mongotmock";
        if (!pathExists(this.dataDir)) {
            resetDbpath(this.dataDir);
        }
        // Select the correct ingress listening port to communicate with based on whether or not
        // we are using gRPC or MongoRPC to communicate.
        this.dataDir = this.dataDir + "/" + (this.useGRPC() ? this.gRPCPort : this.port);
        resetDbpath(this.dataDir);
    }

    /**
     *  Start mongotmock and wait for it to start.
     */
    start(opts = {bypassAuth: false, tlsMode: "disabled"}) {
        print("mongotmock: " + (this.useGRPC() ? this.gRPCPort : this.port));
        const tlsEnabled = tlsModeOptions.includes(opts.tlsMode) && opts.tlsMode != "disabled";

        // The search_ssl suite automatically enables TLS for connections to mongo processes,
        // including mongotmock. We need to control TLS usage for mongotmock connections to test
        // scenarios with TLS disabled on mongot but enabled on mongod. Therefore, we use host:port
        // configurations when enabling TLS on mongot, and use mongocryptd.sock otherwise, as it
        // doesn't use TLS for mongotmock connections. Similar to what we do for the dataDir, when
        // using host:port, we must communicate with the ingress port listening for gRPC when
        // useGRPC is true, and communicate with the ingress port expecting MongoRPC otherwise.
        const conn_str = tlsEnabled
            ? "localhost:" + (this.useGRPC() ? this.gRPCPort : this.port)
            : this.dataDir + "/mongocryptd.sock";
        const args = [this.mongotMock];

        args.push("--port=" + this.port);
        if (this.useGRPC()) {
            args.push("--grpcPort=" + this.gRPCPort);
        }
        // mongotmock uses mongocryptd.sock.
        args.push("--unixSocketPrefix=" + this.dataDir);

        if (this.useGRPC()) {
            // We set up mongotmock with ingress gRPC to enable testing the community architecture,
            // but this is still gated behind a feature flag.
            args.push("--setParameter");
            args.push("featureFlagGRPC=1");
        }

        args.push("--setParameter");
        args.push("enableTestCommands=1");
        args.push("-vvv");

        args.push("--pidfilepath=" + this.dataDir + "/cryptd.pid");

        if (tlsEnabled || this.useGRPC()) {
            args.push("--tlsMode");
            if (this.useGRPC() && !tlsEnabled) {
                jsTestLog("Overriding tlsMode=disabled due to mongotmock gRPC server requiring TLS");
                args.push("allowTLS"); // "disabled" is not a valid setting for ingress gRPC
            } else {
                args.push(opts.tlsMode);
            }
            args.push("--tlsCertificateKeyFile");
            args.push(SERVER_CERT);
            args.push("--tlsCAFile");
            args.push(CA_CERT);
            args.push("--tlsAllowConnectionsWithoutCertificates");
        }

        if (TestData && TestData.auth && !opts.bypassAuth) {
            args.push("--clusterAuthMode=keyFile");
            args.push("--keyFile=" + TestData.keyFile);
        }

        this.pid = _startMongoProgram({args: args});

        assert(checkProgram(this.pid));

        // Wait for connection to be established with server
        let conn = null;
        const pid = this.pid;
        const useGRPC = this.useGRPC();

        assert.soon(
            function () {
                try {
                    if (!tlsEnabled) {
                        conn = new Mongo(conn_str, undefined, {gRPC: useGRPC});
                    } else {
                        conn = new Mongo(conn_str, undefined, {
                            tls: {certificateKeyFile: CLIENT_CERT, CAFile: CA_CERT},
                            gRPC: useGRPC,
                        });
                    }
                    if (TestData && TestData.auth && opts.bypassAuth) {
                        // if Mongot is opting out of auth, we don't need to
                        // authenticate our connection to it.
                        conn.authenticated = true;
                    }
                    conn.pid = pid;
                    return true;
                } catch (e) {
                    let res = checkProgram(pid);
                    if (!res.alive) {
                        print(
                            "Could not start mongo program at " +
                                conn_str +
                                ", process ended with exit code: " +
                                res.exitCode,
                        );
                        return true;
                    }
                }
                return false;
            },
            "unable to connect to mongo program on port " + conn_str,
            30 * 1000,
        );

        this.conn = conn;
        print("mongotmock sucessfully started.");
    }

    /**
     *  Stop mongotmock, asserting that it shutdown cleanly or with the provided code.
     */
    stop(code = kSIGTERM) {
        // Check the remaining history on the mock. There should be 0 remaining queued commands.
        this.assertEmpty();

        return stopMongoProgramByPid(this.pid, code);
    }

    /**
     * Returns a connection to mongotmock.
     */
    getConnection() {
        return this.conn;
    }

    useGRPC() {
        return TestData && TestData.setParameters && TestData.setParameters.useGrpcForSearch;
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
            assert.commandWorked(
                connection.getDB("mongotmock").runCommand({
                    allowMultiCursorResponse: 1,
                    cursorId: NumberLong(additionalCursorId),
                }),
            );
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

    clearQueuedResponses() {
        const connection = this.getConnection();
        assert.commandWorked(connection.getDB("mongotmock").runCommand({clearQueuedResponses: {}}));
        this.assertEmpty();
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

                assert(
                    r.hasOwnProperty("maybeUnused") && r["maybeUnused"],
                    `found unused response for ${cursorId}: ${tojson(r)}`,
                );
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
            manageSearchIndexResponse: expectedManageSearchIndexResponse,
        };
        assert.commandWorked(connection.getDB("mongotmock").runCommand(setManageSearchIndexResponseCommand));
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
        return assert.commandWorked(connection.getDB("mongotmock").runCommand(manageSearchIndexCommand));
    }

    /**
     * Convenience function that instructs the mongotmock to close the incoming connection
     * on which it receives a command in response to the next `n` search commands.
     */
    closeConnectionInResponseToNextNRequests(n) {
        const connection = this.getConnection();
        assert.commandWorked(connection.adminCommand({closeConnectionOnNextRequests: NumberInt(n)}));
    }
}
