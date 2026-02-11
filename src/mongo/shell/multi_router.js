/**
 * SessionMongoMap - Maps session IDs to Mongo connections.
 * Handles UUID-to-string conversion for proper Map lookup
 *
 * The class ensures we have only one pinned mongo per session ID.
 * The class is meant to pin a mongo for a given multi document transaction.
 * The logic is optimized so that a new txnNumber will erase the previous entry
 * as we can assume the previous transaction has terminated.
 */
class SessionMongoMap {
    constructor() {
        this._map = new Map();
    }

    /**
     * Convert a session ID to a string key for the Map
     * @param {Object} sessionId - Session ID object with structure {id: UUID(...)}
     * @returns {string} - Hex string representation of the UUID
     */
    _toKey(sessionId) {
        if (!sessionId || !sessionId.id) {
            throw new Error("Invalid sessionId: " + tojson(sessionId));
        }
        return sessionId.id.hex();
    }

    /**
     * Set a session ID -> Mongo connection mapping.
     * If session ID already exists it will erase it first.
     * @param {Object} sessionId - Session ID object with structure {id: UUID(...)}
     * @param {Mongo} mongo - Mongo connection instance
     */
    set(sessionId, txnNumber, mongo) {
        const key = this._toKey(sessionId);
        if (this.has(sessionId)) {
            this.delete(sessionId);
        }
        this._map.set(key, {txnNumber, mongo});
    }

    /**
     * Get the Mongo connection for a session ID and txnNumber
     * @param {Object} sessionId - Session ID object with structure {id: UUID(...)}
     * @returns {Mongo|undefined} - Mongo connection or undefined if not found
     */
    get(sessionId, txnNumber) {
        const key = this._toKey(sessionId);
        const value = this._map.get(key);
        // If the txnNumber changed we are in a new transaction and we should return a new mongo.
        if (value && Number(value.txnNumber) === Number(txnNumber)) {
            return value.mongo;
        }
        return undefined;
    }

    /**
     * Delete a session ID mapping
     * @param {Object} sessionId - Session ID object with structure {id: UUID(...)}
     */
    delete(sessionId) {
        const key = this._toKey(sessionId);
        return this._map.delete(key);
    }

    /**
     * Check if a session ID is tracked
     * @param {Object} sessionId - Session ID object with structure {id: UUID(...)}
     * @returns {boolean} - True if the session is tracked
     */
    has(sessionId) {
        const key = this._toKey(sessionId);
        return this._map.has(key);
    }

    /**
     * Get the number of tracked sessions
     * @returns {number} - Number of sessions in the map
     */
    size() {
        return this._map.size;
    }

    /**
     * Clear all session mappings
     */
    clear() {
        this._map.clear();
    }

    /**
     * Get string representation for debugging
     * @returns {string}
     */
    toString() {
        const entries = Array.from(this._map.entries())
            .map(([key, mongo]) => `${key}: ${mongo.host}`)
            .join(", ");
        return `SessionMongoMap(${this._map.size} entries) { ${entries} }`;
    }
}

/**
 * CursorTracker - Maps cursor IDs to Mongo connections.
 *
 * This ensures that getMore commands are routed to the same mongos that initiated the cursor.
 *
 * Uses a plain JavaScript object instead of a Map. When a NumberLong is used as a
 * property key on a plain object, JavaScript automatically converts it to a string via toString().
 * This gives us value-based comparison (e.g., "NumberLong(123)") rather than reference-based
 * comparison that Map would use.
 */
class CursorTracker {
    kNoCursor = new NumberLong(0);

    connectionsByCursorId = {};

    getConnectionUsedForCursor(cursorId) {
        return cursorId instanceof NumberLong ? this.connectionsByCursorId[cursorId] : undefined;
    }

    setConnectionUsedForCursor(cursorId, cursorConn) {
        // Skip if it's the "no cursor" sentinel value
        if (cursorId instanceof NumberLong && !bsonBinaryEqual({_: cursorId}, {_: this.kNoCursor})) {
            this.connectionsByCursorId[cursorId] = cursorConn;
        }
    }

    count() {
        return Object.keys(this.connectionsByCursorId).length;
    }
}

function throwCommandNotSupportedError(cmdName, reason, cmd) {
    throw Error(
        `Command ${tojson(cmdName)} is not supported with random mongos dispatching. ${reason}. ` +
            "Please disable mongos dispatching for this test: TestData.pinToSingleMongos = true. Command: " +
            tojson(cmd),
    );
}

function assertIsSupportedCommand(cmd) {
    if (!cmd) return;
    if (cmd.releaseMemory) {
        if (cmd.releaseMemory.length > 1) {
            throwCommandNotSupportedError(
                "releaseMemory with multiple cursors",
                "Cursors may reside on different mongos instances and we currently don't support driver-side merging of results",
                cmd,
            );
        }
    }
    if (cmd.killCursors) {
        if (cmd.cursors && cmd.cursors.length > 1) {
            throwCommandNotSupportedError(
                "killCursors with multiple cursors",
                "Cursors may reside on different mongos instances and we currently don't support driver-side merging of results",
                cmd,
            );
        }
    }
    if (cmd.aggregate && Array.isArray(cmd.pipeline)) {
        for (const stage of cmd.pipeline) {
            // stages might be null if the pipeline is malformed.
            if (!stage) {
                continue;
            }
            if (stage.$currentOp && stage.$currentOp.localOps === true) {
                throwCommandNotSupportedError(
                    "'$currentOp' with 'localOps: true'",
                    "It targets local operations on a specific mongos instance",
                    cmd,
                );
            }
            if (stage.$listLocalSessions) {
                throwCommandNotSupportedError(
                    "'$listLocalSessions'",
                    "It targets local sessions on a specific mongos instance",
                    cmd,
                );
            }
        }
    }
    if (cmd.getShardVersion) {
        throwCommandNotSupportedError("getShardVersion", "It targets a specific mongos instance", cmd);
    }
    if (cmd.getDatabaseVersion) {
        throwCommandNotSupportedError("getDatabaseVersion", "It targets a specific mongos instance", cmd);
    }
    if (cmd.getLog) {
        throwCommandNotSupportedError("getLog", "It targets a specific mongos instance", cmd);
    }
}

// Returns whether the command either set or remove a query settings commands.
function isQuerySettingsCommand(cmd) {
    return cmd.setQuerySettings || cmd.removeQuerySettings;
}

// Returns whether the command must be broadcasted to all mongoses instead of just one.
function requiresBroadcast(cmd) {
    if (cmd.refreshLogicalSessionCacheNow) {
        return true;
    }
    if (cmd.flushRouterConfig) {
        return true;
    }
    // setParameter is node-specific and in every core test the command sets the parameter on a mongos.
    // To run setParameter against a shard there are specific helpers to do so.
    // In production, a user must choose the node that requires that server parameter.
    // For the jscore tests, broadcasting  makes sense because it allows to retrieve its value
    // or sets a specific mongos behaviour indipendently on the mongos chosen.
    if (cmd.setParameter) {
        return true;
    }
    return false;
}

function extractCursorID(cmd) {
    if (!cmd) return undefined;
    if (cmd.getMore) {
        return cmd.getMore;
    }
    if (cmd.releaseMemory) {
        return cmd.releaseMemory[0];
    }
    if (cmd.killCursors && cmd.cursors) {
        return cmd.cursors[0];
    }
    return undefined;
}

/**
 * Converts a MongoDB URI object into an array of individual URIs, one for each server.
 *
 * @param {Object} mongouri - MongoDB URI object with servers, database, and options
 * @returns {Array<string>} Array of MongoDB connection URIs
 * [
 *  "mongodb://localhost:20044/test?compressors=disabled",
 *  "mongodb://localhost:20045/test?compressors=disabled"
 *   ...
 * ]
 */
function toConnectionsList(mongouri) {
    const optionsString =
        Object.keys(mongouri.options).length > 0
            ? "?" +
              Object.entries(mongouri.options)
                  .map(([key, val]) => `${key}=${val}`)
                  .join("&")
            : "";

    return mongouri.servers.map((s) => `mongodb://${s.server}/${mongouri.database}${optionsString}`);
}

/**
 * MultiRouterMongo
 *
 * This class holds a connection pool of mongoses and dispatches commands randomly against it.
 * It ensures that stateful operations (transactions, cursors) are pinned to the same mongos
 * while allowing stateless operations to be distributed randomly.
 *
 * Signature matches the Mongo constructor: (url, encryptedDBClientCallback, apiParameters)
 */
function MultiRouterMongo(uri, encryptedDBClientCallback, apiParameters) {
    const mongoURI = new MongoURI(uri);

    if (mongoURI.servers.length === 0) {
        throw Error("No mongos hosts found in connection string");
    }

    // ============================================================================
    // Logging
    // ============================================================================

    this._name = "multi_router_number_" + Math.floor(Math.random() * 100000);

    this.log = function (text) {
        chatty("[" + this._name + "] " + text);
    };

    // ============================================================================
    // Connection pool and primary connection
    // ============================================================================

    this.log("Establishing Multi-Router Mongo connector... uri: " + uri);
    const individualURIs = toConnectionsList(mongoURI);
    this._mongoConnections = individualURIs.map((uri) => {
        return new Mongo(uri, encryptedDBClientCallback, apiParameters);
    });

    for (const mongo of this._mongoConnections) {
        const res = assert.commandWorked(mongo._getDefaultSession().getClient().adminCommand("ismaster"));
        if ("isdbgrid" !== res.msg) {
            throw new Error(
                "Multi-Router Mongo connector failed. Connection against " + mongo.host + "is not a mongos",
            );
        }
    }

    // Selects a random mongo from the connection pool
    this._getNextMongo = function () {
        let normalizedRandValue = Math.random();
        let randomIndex = Math.floor(normalizedRandValue * this._mongoConnections.length);
        return this._mongoConnections[randomIndex];
    };

    // The primary mongo is a pinned connection that the proxy falls back on when
    // the workload should not be distributed. This is useful for:
    // - Tests that require dispatching to be disabled
    // - Executing getters or setters on options
    // - Executing getters or setters on the cluster logicalTime
    // The primary mongo acts as the holder of shared state among the connection pool.
    this.primaryMongo = this._getNextMongo();
    this.isMultiRouter = true;
    this.uri = uri;

    this.log("Established a Multi-Router Mongo connector. Mongos connections list: " + individualURIs);

    // ============================================================================
    // State tracking (cursors and sessions)
    // ============================================================================

    this._cursorTracker = new CursorTracker();
    this._sessionToMongoMap = new SessionMongoMap(this._name);

    // ============================================================================
    // Field Level Encryption (FLE) methods
    // ============================================================================

    this.setAutoEncryption = function (fleOptions) {
        let res;
        for (const mongo of this._mongoConnections) {
            res = mongo.setAutoEncryption(fleOptions);
            if (!res) {
                return res;
            }
        }
        return res;
    };

    this.toggleAutoEncryption = function (flag) {
        let res;
        for (const mongo of this._mongoConnections) {
            res = mongo.toggleAutoEncryption(flag);
            if (!res) {
                return res;
            }
        }
        return res;
    };

    this.unsetAutoEncryption = function () {
        let res;
        for (const mongo of this._mongoConnections) {
            res = mongo.unsetAutoEncryption();
            if (!res) {
                return res;
            }
        }
        return res;
    };

    // ============================================================================
    // Command routing (core routing logic)
    // ============================================================================

    // Broadcast the command to all mongoses and returns error if any returns error.
    this.broadcast = function (dbName, cmd) {
        let res;
        for (const mongo of this._mongoConnections) {
            res = mongo.getDB(dbName).runCommand(cmd);
            if (!res.ok) {
                return res;
            }
        }
        return res;
    };

    this.refreshClusterParameters = function () {
        // This will force the refresh of all the cluster parameters
        return this.broadcast("admin", {getClusterParameter: "*"});
    };

    this.setLogLevel = function (logLevel, component, session) {
        let result;
        for (const mongo of this._mongoConnections) {
            result = mongo.setLogLevel(logLevel, component, session);
            if (!result.ok) {
                return result;
            }
        }
        return result;
    };

    this.runCommand = function (dbname, cmd, options) {
        // Ensure we call this overriden runCommand if pinToSingleMongos is undefined or disabled.
        assert.neq(TestData.pinToSingleMongos, true);

        assertIsSupportedCommand(cmd);

        if (requiresBroadcast(cmd)) {
            return this.broadcast(dbname, cmd);
        }

        let mongo;

        // Multi-document transactions must use the same mongos
        // Extract the first connection randomly and pin it for subsequent use
        if (cmd && cmd.lsid && cmd.txnNumber) {
            mongo = this._sessionToMongoMap.get(cmd.lsid, cmd.txnNumber);
            if (!mongo) {
                let sessionInfo = {sessionId: cmd.lsid, txnNuber: cmd.txnNumber};
                this.log("Found no mongo for the multi-document transaction: " + tojson(sessionInfo));
                mongo = this._getNextMongo();
                // This will erase the previous entry for the same session id but different txnNumber
                this._sessionToMongoMap.set(cmd.lsid, cmd.txnNumber, mongo);
            }
        }

        // some query commands must use the same mongos that initiated the cursor
        const cursorID = extractCursorID(cmd);
        if (cursorID) {
            mongo = this._cursorTracker.getConnectionUsedForCursor(cursorID);
            if (!mongo) {
                // While a mongo is expected to be found, there are tests that specifically run getMore on non-existent cursors.
                // For this case any mongo is equivalent because the command is only expecting to fail.
                mongo = this.primaryMongo;
            }
        }

        // If no mongo has been selected yet, pick one randomly
        if (!mongo) {
            mongo = this._getNextMongo();
        }

        const result = mongo.runCommand(dbname, cmd, options);

        // Track cursor-to-mongos mapping for aggregations and finds
        // After extracting the first connection randomly, we pin it for subsequent getMore commands
        if (result && result.cursor && result.cursor.id && !cmd.getMore) {
            this._cursorTracker.setConnectionUsedForCursor(result.cursor.id, mongo);
        }

        if (isQuerySettingsCommand(cmd) && result && result.ok) {
            // Refresh the cluster parameters on all connections to propagate query settings changes
            this.refreshClusterParameters();
        }

        if (this.shouldDisableMultiRouter_TEMPORARY_WORKAROUND(cmd)) {
            // Self disable the multi-router.
            // Starting from the next runCommand, any command will run against the primary mongo until the end of the test.
            TestData.pinToSingleMongos = true;
        }
        return result;
    };

    // ============================================================================
    // Session management
    // ============================================================================

    this.startSession = function (options = {}, proxy) {
        if (!options.hasOwnProperty("retryWrites") && this.primaryMongo.hasOwnProperty("_retryWrites")) {
            options.retryWrites = this.primaryMongo._retryWrites;
        }

        const newDriverSession = new DriverSession(proxy, options);

        if (typeof TestData === "object" && TestData.testName) {
            print(
                "New session started with sessionID: " +
                    tojsononeline(newDriverSession.getSessionId()) +
                    " and options: " +
                    tojsononeline(options),
            );
        }

        return newDriverSession;
    };

    this._getDefaultSession = function (proxy) {
        if (!this.hasOwnProperty("_defaultSession")) {
            if (_shouldUseImplicitSessions()) {
                try {
                    this._defaultSession = this.startSession({causalConsistency: false}, proxy);
                } catch (e) {
                    if (e instanceof DriverSession.UnsupportedError) {
                        chatty("WARNING: No implicit session: " + e.message);
                        this._defaultSession = new _DummyDriverSession(proxy);
                    } else {
                        print("ERROR: Implicit session failed: " + e.message);
                        throw e;
                    }
                }
            } else {
                this._defaultSession = new _DummyDriverSession(proxy);
            }
            this._defaultSession._isExplicit = false;
        }
        return this._defaultSession;
    };

    // ============================================================================
    // Database and admin operations
    // ============================================================================

    this.getDB = function (name, proxy) {
        // Note this implementations allows to capture any overridden implementation of Mongo.prototype.getDB from custom hooks (e.g. enable_sessions.js)
        let newDb = this.primaryMongo.getDB(name);
        newDb._mongo = proxy;
        return newDb;
    };

    this.adminCommand = function (cmd, proxy) {
        return this.getDB("admin", proxy).runCommand(cmd);
    };

    // ============================================================================
    // Authentication methods
    // ============================================================================

    this.logout = function (dbname) {
        let result;
        for (const mongo of this._mongoConnections) {
            result = mongo.logout(dbname);
            if (!result.ok) {
                return result;
            }
        }
        return result;
    };

    this.auth = function (params) {
        let result;
        for (const mongo of this._mongoConnections) {
            result = mongo.auth(params);
            if (!result) {
                return result;
            }
        }
        return result;
    };

    // ============================================================================
    // Utility methods
    // ============================================================================

    this.isConnectedToMongos = function () {
        // Assert the primary Mongo is connected to a mongos
        const res = assert.commandWorked(this.primaryMongo._getDefaultSession().getClient().adminCommand("ismaster"));
        return "isdbgrid" === res.msg;
    };

    // For every logic within this function there must be associated a TODO ticket.
    // Check if the current command is currently unsupported due to a necesserary fix.
    this.shouldDisableMultiRouter_TEMPORARY_WORKAROUND = function (cmd) {
        // TODO (SERVER-115554) remove this check once the described issue is solved in v8.0
        // Timeseries in v8.0 might fail to insert if the routing cache is stale.
        // We currently don't support timeseries insertions in multi-version on multi-router.
        let disable = false;
        if (cmd && cmd.create && cmd.timeseries && TestData.mongosBinVersion) {
            chatty("Disabling Multi-Router connector - the command is temporarily unsupported" + tojson(cmd));
            disable = true;
        }
        return disable;
    };

    // TODO SERVER-116289 Remove this helper
    this.hasPrimaryMongoRefreshed = false;
    this.refreshPrimaryMongoIfNeeded = function () {
        if (!this.hasPrimaryMongoRefreshed) {
            assert.commandWorked(this.primaryMongo.adminCommand({flushRouterConfig: 1}));
            this.hasPrimaryMongoRefreshed = true;
        }
    };

    // ============================================================================
    // Proxy handler
    // ============================================================================
    return new Proxy(this, {
        get(target, prop, proxy) {
            // If the proxy is disabled by the test, always run the command on the pinned mongos (primary mongo).
            // TODO (SERVER-116289) This refresh is required because some tests keep failing in spite all commands being routed to a single mongos.
            // Remove this refresh (if possible) once the underling reason is solved.
            if (jsTest.options().pinToSingleMongos) {
                target.refreshPrimaryMongoIfNeeded();
                const value = target.primaryMongo[prop];
                if (typeof value === "function") {
                    return value.bind(target.primaryMongo);
                }
                return value;
            }

            if (prop === "_runCommandImpl") {
                throw new Error("You should never run _runCommandImpl against the proxy but always against a Mongo!");
            }

            if (prop === "adminCommand") {
                return function (cmd) {
                    return target.adminCommand(cmd, proxy);
                };
            }

            if (prop === "getMongo") {
                return function () {
                    return proxy;
                };
            }

            if (prop === "defaultDB") {
                return target.getDB("test", proxy);
            }

            if (prop === "getDB") {
                return function (name) {
                    return target.getDB(name, proxy);
                };
            }

            if (prop === "_getDefaultSession") {
                return function () {
                    return target._getDefaultSession(proxy);
                };
            }

            if (prop === "startSession") {
                return function (options) {
                    return target.startSession(options, proxy);
                };
            }

            if (prop === "host") {
                // Return a random host for backward compatibility. For a full multi-host URI that
                // can be passed to the "connect" function, use the 'uri' property instead.
                return target._getNextMongo().host;
            }

            if (target.hasOwnProperty(prop)) {
                return target[prop];
            }

            // For every un-implemented property, run on primary mongo
            const value = target.primaryMongo[prop];
            if (typeof value === "function") {
                return value.bind(target.primaryMongo);
            }
            return value;
        },

        set(target, prop, value, receiver) {
            target[prop] = value;
            return true;
        },
    });
}

export {MultiRouterMongo, toConnectionsList};
