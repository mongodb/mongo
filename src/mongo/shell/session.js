/**
 * Implements the sessions api for the shell.
 */
var {
    DriverSession, SessionOptions, _DummyDriverSession,
} = (function() {
    "use strict";

    function SessionOptions(rawOptions = {}) {
        let _readPreference = rawOptions.readPreference;
        let _readConcern = rawOptions.readConcern;
        let _writeConcern = rawOptions.writeConcern;
        const _initialClusterTime = rawOptions.initialClusterTime;
        const _initialOperationTime = rawOptions.initialOperationTime;
        let _causalConsistency = rawOptions.causalConsistency;
        let _retryWrites = rawOptions.retryWrites;

        this.getReadPreference = function getReadPreference() {
            return _readPreference;
        };

        this.setReadPreference = function setReadPreference(readPreference) {
            _readPreference = readPreference;
        };

        this.getReadConcern = function getReadConcern() {
            return _readConcern;
        };

        this.setReadConcern = function setReadConcern(readConcern) {
            _readConcern = readConcern;
        };

        this.getWriteConcern = function getWriteConcern() {
            return _writeConcern;
        };

        this.setWriteConcern = function setWriteConcern(writeConcern) {
            if (!(writeConcern instanceof WriteConcern)) {
                writeConcern = new WriteConcern(writeConcern);
            }
            _writeConcern = writeConcern;
        };

        this.getInitialClusterTime = function getInitialClusterTime() {
            return _initialClusterTime;
        };

        this.getInitialOperationTime = function getInitialOperationTime() {
            return _initialOperationTime;
        };

        this.isCausalConsistency = function isCausalConsistency() {
            return _causalConsistency;
        };

        this.setCausalConsistency = function setCausalConsistency(causalConsistency = true) {
            _causalConsistency = causalConsistency;
        };

        this.shouldRetryWrites = function shouldRetryWrites() {
            return _retryWrites;
        };

        this.setRetryWrites = function setRetryWrites(retryWrites = true) {
            _retryWrites = retryWrites;
        };
    }

    function SessionAwareClient(client) {
        const kWireVersionSupportingLogicalSession = 6;
        const kWireVersionSupportingCausalConsistency = 6;

        this.getReadPreference = function getReadPreference(driverSession) {
            const sessionOptions = driverSession.getOptions();
            if (sessionOptions.getReadPreference() !== undefined) {
                return sessionOptions.getReadPreference();
            }
            return client.getReadPref();
        };

        this.getReadConcern = function getReadConcern(driverSession) {
            const sessionOptions = driverSession.getOptions();
            if (sessionOptions.getReadConcern() !== undefined) {
                return sessionOptions.getReadConcern();
            }
            if (client.getReadConcern() !== undefined) {
                return {level: client.getReadConcern()};
            }
            return null;
        };

        this.getWriteConcern = function getWriteConcern(driverSession) {
            const sessionOptions = driverSession.getOptions();
            if (sessionOptions.getWriteConcern() !== undefined) {
                return sessionOptions.getWriteConcern();
            }
            return client.getWriteConcern();
        };

        function serverSupports(wireVersion) {
            return client.getMinWireVersion() <= wireVersion &&
                wireVersion <= client.getMaxWireVersion();
        }

        const kCommandsThatSupportReadConcern = new Set([
            "aggregate",
            "count",
            "distinct",
            "explain",
            "find",
            "geoNear",
            "geoSearch",
            "group",
            "mapReduce",
            "mapreduce",
            "parallelCollectionScan",
        ]);

        function canUseReadConcern(cmdObj) {
            let cmdName = Object.keys(cmdObj)[0];

            // If the command is in a wrapped form, then we look for the actual command name inside
            // the query/$query object.
            let cmdObjUnwrapped = cmdObj;
            if (cmdName === "query" || cmdName === "$query") {
                cmdObjUnwrapped = cmdObj[cmdName];
                cmdName = Object.keys(cmdObjUnwrapped)[0];
            }

            if (!kCommandsThatSupportReadConcern.has(cmdName)) {
                return false;
            }

            if (cmdName === "explain") {
                return kCommandsThatSupportReadConcern.has(Object.keys(cmdObjUnwrapped.explain)[0]);
            }

            return true;
        }

        function injectAfterClusterTime(cmdObj, operationTime) {
            cmdObj = Object.assign({}, cmdObj);

            if (operationTime !== undefined) {
                const cmdName = Object.keys(cmdObj)[0];

                // If the command is in a wrapped form, then we look for the actual command object
                // inside the query/$query object.
                let cmdObjUnwrapped = cmdObj;
                if (cmdName === "query" || cmdName === "$query") {
                    cmdObj[cmdName] = Object.assign({}, cmdObj[cmdName]);
                    cmdObjUnwrapped = cmdObj[cmdName];
                }

                cmdObjUnwrapped.readConcern = Object.assign({}, cmdObjUnwrapped.readConcern);
                const readConcern = cmdObjUnwrapped.readConcern;

                if (!readConcern.hasOwnProperty("afterClusterTime")) {
                    readConcern.afterClusterTime = operationTime;
                }
            }

            return cmdObj;
        }

        function prepareCommandRequest(driverSession, cmdObj) {
            if (serverSupports(kWireVersionSupportingLogicalSession)) {
                cmdObj = driverSession._serverSession.injectSessionId(cmdObj);
            }

            if (serverSupports(kWireVersionSupportingCausalConsistency) &&
                (driverSession.getOptions().isCausalConsistency() ||
                 client.isCausalConsistency()) &&
                canUseReadConcern(cmdObj)) {
                // `driverSession._operationTime` is the smallest time needed for performing a
                // causally consistent read using the current session. Note that
                // `client.getClusterTime()` is no smaller than the operation time and would
                // therefore only be less efficient to wait until.
                cmdObj = injectAfterClusterTime(cmdObj, driverSession._operationTime);
            }

            return cmdObj;
        }

        function processCommandResponse(driverSession, res) {
            if (res.hasOwnProperty("operationTime") &&
                bsonWoCompare({_: res.operationTime}, {_: driverSession._operationTime}) > 0) {
                driverSession._operationTime = res.operationTime;
            }

            if (res.hasOwnProperty("$clusterTime")) {
                client.setClusterTime(res.$clusterTime);
            }
        }

        function runClientFunctionWithRetries(
            driverSession, cmdObj, clientFunction, clientFunctionArguments) {
            let cmdName = Object.keys(cmdObj)[0];

            // If the command is in a wrapped form, then we look for the actual command object
            // inside the query/$query object.
            if (cmdName === "query" || cmdName === "$query") {
                cmdObj = cmdObj[cmdName];
                cmdName = Object.keys(cmdObj)[0];
            }

            let numRetries =
                (cmdObj.hasOwnProperty("txnNumber") && !jsTest.options().skipRetryOnNetworkError)
                ? 1
                : 0;

            do {
                try {
                    return clientFunction.apply(client, clientFunctionArguments);
                } catch (e) {
                    // TODO: Should we run an explicit "isMaster" command in order to compare the
                    // wire version of the server after we reconnect to it?
                    if (!isNetworkError(e) || numRetries === 0) {
                        throw e;
                    }
                }

                --numRetries;
            } while (numRetries >= 0);
        }

        this.runCommand = function runCommand(driverSession, dbName, cmdObj, options) {
            cmdObj = prepareCommandRequest(driverSession, cmdObj);

            const res = runClientFunctionWithRetries(
                driverSession, cmdObj, client.runCommand, [dbName, cmdObj, options]);

            processCommandResponse(driverSession, res);
            return res;
        };

        this.runCommandWithMetadata = function runCommandWithMetadata(
            driverSession, dbName, metadata, cmdObj) {
            cmdObj = prepareCommandRequest(driverSession, cmdObj);

            const res = runClientFunctionWithRetries(
                driverSession, cmdObj, client.runCommandWithMetadata, [dbName, metadata, cmdObj]);

            processCommandResponse(driverSession, res);
            return res;
        };
    }

    function ServerSession(client) {
        let _lastUsed = new Date();
        let _nextTxnNum = 0;

        this.client = new SessionAwareClient(client);
        this.handle = client._startSession();

        this.getLastUsed = function getLastUsed() {
            return _lastUsed;
        };

        function updateLastUsed() {
            _lastUsed = new Date();
        }

        this.injectSessionId = function injectSessionId(cmdObj) {
            cmdObj = Object.assign({}, cmdObj);

            const cmdName = Object.keys(cmdObj)[0];

            // If the command is in a wrapped form, then we look for the actual command object
            // inside the query/$query object.
            let cmdObjUnwrapped = cmdObj;
            if (cmdName === "query" || cmdName === "$query") {
                cmdObj[cmdName] = Object.assign({}, cmdObj[cmdName]);
                cmdObjUnwrapped = cmdObj[cmdName];
            }

            if (!cmdObjUnwrapped.hasOwnProperty("lsid")) {
                cmdObjUnwrapped.lsid = this.handle.getId();

                // We consider the session to still be in used by the client any time the session id
                // is injected into the command object as part of making a request.
                updateLastUsed();
            }

            return cmdObj;
        };

        this.assignTransactionNumber = function assignTransactionNumber(cmdObj) {
            cmdObj = Object.assign({}, cmdObj);

            const cmdName = Object.keys(cmdObj)[0];

            // If the command is in a wrapped form, then we look for the actual command object
            // inside the query/$query object.
            let cmdObjUnwrapped = cmdObj;
            if (cmdName === "query" || cmdName === "$query") {
                cmdObj[cmdName] = Object.assign({}, cmdObj[cmdName]);
                cmdObjUnwrapped = cmdObj[cmdName];
            }

            if (!cmdObjUnwrapped.hasOwnProperty("txnNumber")) {
                // Since there's no native support for adding NumberLong instances and getting back
                // another NumberLong instance, converting from a 64-bit floating-point value to a
                // 64-bit integer value will overflow at 2**53.
                cmdObjUnwrapped.txnNumber = new NumberLong(_nextTxnNum);
                ++_nextTxnNum;
            }

            return cmdObj;
        };

        this.canRetryWrites = function canRetryWrites(cmdObj) {
            let cmdName = Object.keys(cmdObj)[0];

            // If the command is in a wrapped form, then we look for the actual command name inside
            // the query/$query object.
            if (cmdName === "query" || cmdName === "$query") {
                cmdObj = cmdObj[cmdName];
                cmdName = Object.keys(cmdObj)[0];
            }

            if (cmdName === "insert") {
                if (!Array.isArray(cmdObj.documents)) {
                    // The command object is malformed, so we'll just leave it as-is and let the
                    // server reject it.
                    return false;
                }

                if (cmdObj.documents.length === 1) {
                    // Single-statement operations (e.g. insertOne()) can be retried.
                    return true;
                }

                // Multi-statement operations (e.g. insertMany()) can be retried if they are
                // executed in order by the server.
                return cmdObj.ordered ? true : false;
            } else if (cmdName === "update") {
                if (!Array.isArray(cmdObj.updates)) {
                    // The command object is malformed, so we'll just leave it as-is and let the
                    // server reject it.
                    return false;
                }

                const hasMultiUpdate = cmdObj.updates.some(updateOp => updateOp.multi);
                if (hasMultiUpdate) {
                    // Operations that modify multiple documents (e.g. updateMany()) cannot be
                    // retried.
                    return false;
                }

                if (cmdObj.updates.length === 1) {
                    // Single-statement operations that modify a single document (e.g. updateOne())
                    // can be retried.
                    return true;
                }

                // Multi-statement operations that each modify a single document (e.g. bulkWrite())
                // can be retried if they are executed in order by the server.
                return cmdObj.ordered ? true : false;
            } else if (cmdName === "delete") {
                if (!Array.isArray(cmdObj.deletes)) {
                    // The command object is malformed, so we'll just leave it as-is and let the
                    // server reject it.
                    return false;
                }

                // We use bsonWoCompare() in order to handle cases where the limit is specified as a
                // NumberInt() or NumberLong() instance.
                const hasMultiDelete = cmdObj.deletes.some(
                    deleteOp => bsonWoCompare({_: deleteOp.limit}, {_: 0}) === 0);
                if (hasMultiDelete) {
                    // Operations that modify multiple documents (e.g. deleteMany()) cannot be
                    // retried.
                    return false;
                }

                if (cmdObj.deletes.length === 1) {
                    // Single-statement operations that modify a single document (e.g. deleteOne())
                    // can be retried.
                    return true;
                }

                // Multi-statement operations that each modify a single document (e.g. bulkWrite())
                // can be retried if they are executed in order by the server.
                return cmdObj.ordered ? true : false;
            } else if (cmdName === "findAndModify" || cmdName === "findandmodify") {
                // Operations that modify a single document (e.g. findOneAndUpdate()) can be
                // retried.
                return true;
            }

            return false;
        };
    }

    function makeDriverSessionConstructor(implMethods) {
        return function(client, options = {}) {
            let _options = options;
            let _hasEnded = false;

            if (!(_options instanceof SessionOptions)) {
                _options = new SessionOptions(_options);
            }

            this._serverSession = implMethods.createServerSession(client);
            this._operationTime = _options.getInitialOperationTime();

            if (_options.getInitialClusterTime() !== undefined) {
                client.setClusterTime(_options.getInitialClusterTime());
            }

            this.getClient = function getClient() {
                return client;
            };

            this.getOptions = function getOptions() {
                return _options;
            };

            this.getOperationTime = function getOperationTime() {
                return this._operationTime;
            };

            this.getClusterTime = function getClusterTime() {
                return client.getClusterTime();
            };

            this.getDatabase = function getDatabase(dbName) {
                const db = client.getDB(dbName);
                db._session = this;
                return db;
            };

            this.hasEnded = function hasEnded() {
                return _hasEnded;
            };

            this.endSession = function endSession() {
                if (this._hasEnded) {
                    return;
                }

                this._hasEnded = true;
                implMethods.endSession(this._serverSession);
            };
        };
    }

    const DriverSession = makeDriverSessionConstructor({
        createServerSession: function createServerSession(client) {
            return new ServerSession(client);
        },

        endSession: function endSession(serverSession) {
            serverSession.handle.end();
        },
    });

    const DummyDriverSession = makeDriverSessionConstructor({
        createServerSession: function createServerSession(client) {
            return {
                client: new SessionAwareClient(client),

                injectSessionId: function injectSessionId(cmdObj) {
                    return cmdObj;
                },

                assignTransactionNumber: function assignTransactionNumber(cmdObj) {
                    return cmdObj;
                },

                canRetryWrites: function canRetryWrites(cmdObj) {
                    return false;
                },
            };
        },

        endSession: function endSession(serverSession) {},
    });

    // We don't actually put anything on DriverSession.prototype, but this way
    // `session instanceof DriverSession` will work for DummyDriverSession instances.
    DummyDriverSession.prototype = Object.create(DriverSession.prototype);
    DummyDriverSession.prototype.constructor = DriverSession;

    return {
        DriverSession: DriverSession,
        SessionOptions: SessionOptions,
        _DummyDriverSession: DummyDriverSession,
    };
})();
