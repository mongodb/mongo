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

    const individualURIs = toConnectionsList(mongoURI);
    this._mongoConnections = individualURIs.map((uri) => {
        return new Mongo(uri, encryptedDBClientCallback, apiParameters);
    });

    // The primary mongo is a pinned connection that the proxy falls back on when
    // the workload should not be distributed. This is useful for:
    // - Tests that require dispatching to be disabled
    // - Executing getters or setters on options
    // - Executing getters or setters on the cluster logicalTime
    // The primary mongo acts as the holder of shared state among the connection pool.
    this.primaryMongo = this._mongoConnections[0];
    this.defaultDB = this.primaryMongo.defaultDB;
    this.isMultiRouter = true;
    this.host = mongoURI.servers.map((s) => s.server).join(",");

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
        this._mongoConnections.forEach((mongo) => {
            res = mongo.setAutoEncryption(fleOptions);
            if (!res) {
                return res;
            }
        });
        return res;
    };

    this.toggleAutoEncryption = function (flag) {
        let res;
        this._mongoConnections.forEach((mongo) => {
            res = mongo.toggleAutoEncryption(flag);
            if (!res) {
                return res;
            }
        });
        return res;
    };

    this.unsetAutoEncryption = function () {
        let res;
        this._mongoConnections.forEach((mongo) => {
            res = mongo.unsetAutoEncryption();
            if (!res) {
                return res;
            }
        });
        return res;
    };

    // ============================================================================
    // Command routing (core routing logic)
    // ============================================================================

    // Selects a random mongo from the connection pool
    this._getNextMongo = function () {
        let normalizedRandValue = Math.random();
        let randomIndex = Math.floor(normalizedRandValue * this._mongoConnections.length);
        return this._mongoConnections[randomIndex];
    };

    this.runCommand = function (dbname, cmd, options) {
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

        // getMore commands must use the same mongos that initiated the cursor
        if (cmd && cmd.getMore) {
            mongo = this._cursorTracker.getConnectionUsedForCursor(cmd.getMore);
            if (!mongo) {
                throw new Error("Found no mongo for getMore, but we should");
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
        return new DB(proxy, name);
    };

    this.adminCommand = function (cmd, proxy) {
        return new DB(proxy, "admin").runCommand(cmd);
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

    // ============================================================================
    // Proxy handler
    // ============================================================================

    return new Proxy(this, {
        get(target, prop, proxy) {
            // If the proxy is disabled by the test, always run the command on the pinned mongos (primary mongo)
            if (jsTest.options().skipMultiRouterRotation) {
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
