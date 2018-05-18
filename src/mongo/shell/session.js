/**
 * Implements the sessions api for the shell.
 *
 * Roughly follows the driver sessions spec:
 * https://github.com/mongodb/specifications/blob/master/source/sessions/driver-sessions.rst#abstract
 */
var {
    DriverSession, SessionOptions, _DummyDriverSession, _DelegatingDriverSession,
} = (function() {
    "use strict";

    const kShellDefaultShouldRetryWrites =
        typeof _shouldRetryWrites === "function" ? _shouldRetryWrites() : false;

    function isNonNullObject(obj) {
        return typeof obj === "object" && obj !== null;
    }

    function SessionOptions(rawOptions = {}) {
        if (!(this instanceof SessionOptions)) {
            return new SessionOptions(rawOptions);
        }

        let _readPreference = rawOptions.readPreference;
        let _readConcern = rawOptions.readConcern;
        let _writeConcern = rawOptions.writeConcern;

        // Causal consistency is implicitly enabled when a session is explicitly started.
        const _causalConsistency =
            rawOptions.hasOwnProperty("causalConsistency") ? rawOptions.causalConsistency : true;

        // If the user specified --retryWrites to the mongo shell, then we enable retryable
        // writes automatically.
        const _retryWrites = rawOptions.hasOwnProperty("retryWrites")
            ? rawOptions.retryWrites
            : kShellDefaultShouldRetryWrites;

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

        this.isCausalConsistency = function isCausalConsistency() {
            return _causalConsistency;
        };

        this.shouldRetryWrites = function shouldRetryWrites() {
            return _retryWrites;
        };

        this.shellPrint = function _shellPrint() {
            return this.toString();
        };

        this.tojson = function _tojson(...args) {
            return tojson(rawOptions, ...args);
        };

        this.toString = function toString() {
            return "SessionOptions(" + this.tojson() + ")";
        };
    }

    function SessionAwareClient(client) {
        const kWireVersionSupportingCausalConsistency = 6;
        const kWireVersionSupportingLogicalSession = 6;
        const kWireVersionSupportingRetryableWrites = 6;

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

        // TODO: Update this whitelist, or convert it to a blacklist depending on the outcome of
        // SERVER-31743.
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

        function gossipClusterTime(cmdObj, clusterTime) {
            cmdObj = Object.assign({}, cmdObj);

            const cmdName = Object.keys(cmdObj)[0];

            // If the command is in a wrapped form, then we look for the actual command object
            // inside the query/$query object.
            let cmdObjUnwrapped = cmdObj;
            if (cmdName === "query" || cmdName === "$query") {
                cmdObj[cmdName] = Object.assign({}, cmdObj[cmdName]);
                cmdObjUnwrapped = cmdObj[cmdName];
            }

            if (!cmdObjUnwrapped.hasOwnProperty("$clusterTime")) {
                cmdObjUnwrapped.$clusterTime = clusterTime;
            }

            return cmdObj;
        }

        function injectAfterClusterTime(cmdObj, operationTime) {
            cmdObj = Object.assign({}, cmdObj);

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

            return cmdObj;
        }

        function prepareCommandRequest(driverSession, cmdObj) {
            if (serverSupports(kWireVersionSupportingLogicalSession)) {
                cmdObj = driverSession._serverSession.injectSessionId(cmdObj);
            }

            if (serverSupports(kWireVersionSupportingCausalConsistency) &&
                (client.isReplicaSetMember() || client.isMongos()) &&
                !jsTest.options().skipGossipingClusterTime) {
                // The `clientClusterTime` is the highest clusterTime observed by any connection
                // within this mongo shell.
                const clientClusterTime = client.getClusterTime();
                // The `sessionClusterTime` is the highest clusterTime tracked by the
                // `driverSession` session and may lag behind `clientClusterTime` if operations on
                // other sessions or connections are advancing the clusterTime.
                const sessionClusterTime = driverSession.getClusterTime();

                // We gossip the greater of the client's clusterTime and the session's clusterTime.
                // If this is the first command being sent on this connection and/or session, then
                // it's possible that either clusterTime hasn't been initialized yet. Additionally,
                // if the user specified a malformed clusterTime as part of initialClusterTime, then
                // we want the server to be the one to reject it and therefore write our comparisons
                // using bsonWoCompare() accordingly.
                if (isNonNullObject(clientClusterTime) || isNonNullObject(sessionClusterTime)) {
                    let clusterTimeToGossip;

                    if (!isNonNullObject(sessionClusterTime)) {
                        clusterTimeToGossip = clientClusterTime;
                    } else if (!isNonNullObject(clientClusterTime)) {
                        clusterTimeToGossip = sessionClusterTime;
                    } else {
                        clusterTimeToGossip =
                            (bsonWoCompare({_: clientClusterTime.clusterTime},
                                           {_: sessionClusterTime.clusterTime}) >= 0)
                            ? clientClusterTime
                            : sessionClusterTime;
                    }

                    cmdObj = gossipClusterTime(cmdObj, clusterTimeToGossip);
                }
            }

            // If startTransaction was called on the session, attach txn number and readConcern.
            // TODO: SERVER-34170 guard this code with a wire version check.
            if (driverSession._serverSession.isInActiveTransaction()) {
                cmdObj = driverSession._serverSession.assignTxnInfo(cmdObj);
            }

            // TODO SERVER-31868: A user should get back an error if they attempt to advance the
            // DriverSession's operationTime manually when talking to a stand-alone mongod. Removing
            // the `(client.isReplicaSetMember() || client.isMongos())` condition will also involve
            // calling resetOperationTime_forTesting() in JavaScript tests that start different
            // cluster types.
            if (serverSupports(kWireVersionSupportingCausalConsistency) &&
                (client.isReplicaSetMember() || client.isMongos()) &&
                (driverSession.getOptions().isCausalConsistency() ||
                 client.isCausalConsistency()) &&
                canUseReadConcern(cmdObj) &&
                (!driverSession._serverSession.isInActiveTransaction() ||
                 driverSession._serverSession.isFirstStatement())) {
                // When we are in a transaction, we must only attach an afterClusterTime to the
                // first statement because readConcern is not allowed in subsequent statements.

                // `driverSession.getOperationTime()` is the smallest time needed for performing a
                // causally consistent read using the current session. Note that
                // `client.getClusterTime()` is no smaller than the operation time and would
                // therefore only be less efficient to wait until.
                const operationTime = driverSession.getOperationTime();
                if (operationTime !== undefined) {
                    cmdObj = injectAfterClusterTime(cmdObj, driverSession.getOperationTime());
                }
            }

            // Retryable writes code should execute only we are not in an active transaction.
            if (jsTest.options().alwaysInjectTransactionNumber &&
                serverSupports(kWireVersionSupportingRetryableWrites) &&
                driverSession.getOptions().shouldRetryWrites() &&
                driverSession._serverSession.canRetryWrites(cmdObj)) {
                cmdObj = driverSession._serverSession.assignTransactionNumber(cmdObj);
            }

            return cmdObj;
        }

        function processCommandResponse(driverSession, res) {
            if (res.hasOwnProperty("operationTime")) {
                driverSession.advanceOperationTime(res.operationTime);
            }

            if (res.hasOwnProperty("$clusterTime")) {
                driverSession.advanceClusterTime(res.$clusterTime);
                client.advanceClusterTime(res.$clusterTime);
            }
        }

        /**
         * Returns true if the error code is retryable, assuming the command is idempotent.
         *
         * The Retryable Writes specification defines a RetryableError as any network error, any of
         * the following error codes, or an error response with a different code containing the
         * phrase "not master" or "node is recovering".
         *
         * https://github.com/mongodb/specifications/blob/5b53e0baca18ba111364d479a37fa9195ef801a6/
         * source/retryable-writes/retryable-writes.rst#terms
         */
        function isRetryableCode(code) {
            return ErrorCodes.isNetworkError(code) || ErrorCodes.isNotMasterError(code) ||
                ErrorCodes.isShutdownError(code) || ErrorCodes.WriteConcernFailed === code;
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

            // TODO SERVER-33921: Revisit how the mongo shell decides whether it should retry a
            // command or not.
            const sessionOptions = driverSession.getOptions();
            let numRetries =
                (sessionOptions.shouldRetryWrites() && cmdObj.hasOwnProperty("txnNumber") &&
                 !jsTest.options().skipRetryOnNetworkError &&
                 !driverSession._serverSession.isInActiveTransaction())
                ? 1
                : 0;

            if (numRetries > 0 && jsTest.options().overrideRetryAttempts) {
                numRetries = jsTest.options().overrideRetryAttempts;
            }

            do {
                let res;

                try {
                    res = clientFunction.apply(client, clientFunctionArguments);
                } catch (e) {
                    if (!isNetworkError(e) || numRetries === 0) {
                        throw e;
                    }

                    // We run an "isMaster" command explicitly to force the underlying DBClient to
                    // reconnect to the server.
                    const res = client.adminCommand({isMaster: 1});
                    if (res.ok !== 1) {
                        throw e;
                    }

                    // It's possible that the server we're connected with after re-establishing our
                    // connection doesn't support retryable writes. If that happens, then we just
                    // return the original network error back to the user.
                    const serverSupportsRetryableWrites = res.hasOwnProperty("minWireVersion") &&
                        res.hasOwnProperty("maxWireVersion") &&
                        res.minWireVersion <= kWireVersionSupportingRetryableWrites &&
                        kWireVersionSupportingRetryableWrites <= res.maxWireVersion;

                    if (!serverSupportsRetryableWrites) {
                        throw e;
                    }
                }

                if (numRetries > 0) {
                    --numRetries;

                    if (res === undefined) {
                        if (jsTest.options().logRetryAttempts) {
                            jsTest.log("Retrying " + cmdName +
                                       " due to network error, subsequent retries remaining: " +
                                       numRetries);
                        }
                        continue;
                    }

                    if (isRetryableCode(res.code)) {
                        if (jsTest.options().logRetryAttempts) {
                            jsTest.log("Retrying " + cmdName + " due to retryable error (code=" +
                                       res.code + "), subsequent retries remaining: " + numRetries);
                        }
                        if (client.isReplicaSetConnection()) {
                            client._markNodeAsFailed(res._mongo.host, res.code, res.errmsg);
                        }
                        continue;
                    }

                    if (Array.isArray(res.writeErrors)) {
                        // If any of the write operations in the batch fails with a retryable error,
                        // then we retry the entire batch.
                        const writeError =
                            res.writeErrors.find((writeError) => isRetryableCode(writeError.code));

                        if (writeError !== undefined) {
                            if (jsTest.options().logRetryAttempts) {
                                jsTest.log("Retrying " + cmdName +
                                           " due to retryable write error (code=" +
                                           writeError.code + "), subsequent retries remaining: " +
                                           numRetries);
                            }
                            if (client.isReplicaSetConnection()) {
                                client._markNodeAsFailed(
                                    res._mongo.host, writeError.code, writeError.errmsg);
                            }
                            continue;
                        }
                    }

                    if (res.hasOwnProperty("writeConcernError") &&
                        isRetryableCode(res.writeConcernError.code)) {
                        if (jsTest.options().logRetryAttempts) {
                            jsTest.log("Retrying " + cmdName +
                                       " due to retryable write concern error (code=" +
                                       res.writeConcernError.code +
                                       "), subsequent retries remaining: " + numRetries);
                        }
                        if (client.isReplicaSetConnection()) {
                            client._markNodeAsFailed(res._mongo.host,
                                                     res.writeConcernError.code,
                                                     res.writeConcernError.errmsg);
                        }
                        continue;
                    }
                }

                return res;
            } while (true);
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

    function TransactionOptions(rawOptions = {}) {
        if (!(this instanceof TransactionOptions)) {
            return new TransactionOptions(rawOptions);
        }

        let _readConcern = rawOptions.readConcern;
        let _writeConcern = rawOptions.writeConcern;

        this.setTxnReadConcern = function setTxnReadConcern(value) {
            _readConcern = value;
        };

        this.getTxnReadConcern = function getTxnReadConcern() {
            return _readConcern;
        };

        this.setTxnWriteConcern = function setTxnWriteConcern(value) {
            _writeConcern = value;
        };

        this.getTxnWriteConcern = function getTxnWriteConcern() {
            return _writeConcern;
        };
    }

    // The server session maintains the state of a transaction, a monotonically increasing txn
    // number, and a transaction's read/write concerns.
    function ServerSession(client) {
        // The default txnState is `inactive` until we call startTransaction.
        let _txnState = ServerSession.TransactionStates.kInactive;

        let _txnOptions;

        // Keep track of the next available statement id of a transaction.
        let _nextStatementId = 0;

        // _txnNumber starts at -1 because when we increment it, the first transaction
        // and retryable write will both have a txnNumber of 0.
        let _txnNumber = -1;
        let _lastUsed = new Date();

        this.client = new SessionAwareClient(client);
        this.handle = client._startSession();

        this.isInActiveTransaction = function isInActiveTransaction() {
            return _txnState === ServerSession.TransactionStates.kActive;
        };

        this.isFirstStatement = function isFirstStatement() {
            return _nextStatementId === 0;
        };

        this.getLastUsed = function getLastUsed() {
            return _lastUsed;
        };

        this.getTxnOptions = function getTxnOptions() {
            return _txnOptions;
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

                // We consider the session to still be in use by the client any time the session id
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
                _txnNumber++;
                cmdObjUnwrapped.txnNumber = new NumberLong(_txnNumber);
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

            if (isNonNullObject(cmdObj.writeConcern)) {
                const writeConcern = cmdObj.writeConcern;

                // We use bsonWoCompare() in order to handle cases where the "w" field is specified
                // as a NumberInt() or NumberLong() instance.
                if (writeConcern.hasOwnProperty("w") &&
                    bsonWoCompare({_: writeConcern.w}, {_: 0}) === 0) {
                    // Unacknowledged writes cannot be retried.
                    return false;
                }
            }

            if (cmdName === "insert") {
                if (!Array.isArray(cmdObj.documents)) {
                    // The command object is malformed, so we'll just leave it as-is and let the
                    // server reject it.
                    return false;
                }

                // Both single-statement operations (e.g. insertOne()) and multi-statement
                // operations (e.g. insertMany()) can be retried regardless of whether they are
                // executed in order by the server.
                return true;
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

                // Both single-statement operations (e.g. updateOne()) and multi-statement
                // operations (e.g. bulkWrite()) can be retried regardless of whether they are
                // executed in order by the server.
                return true;
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

                // Both single-statement operations (e.g. deleteOne()) and multi-statement
                // operations (e.g. bulkWrite()) can be retried regardless of whether they are
                // executed in order by the server.
                return true;
            } else if (cmdName === "findAndModify" || cmdName === "findandmodify") {
                // Operations that modify a single document (e.g. findOneAndUpdate()) can be
                // retried.
                return true;
            }

            return false;
        };

        this.assignTxnInfo = function assignTxnInfo(cmdObj) {
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
                cmdObjUnwrapped.txnNumber = new NumberLong(_txnNumber);
            }

            // All operations of a multi-statement transaction must specify autocommit=false.
            cmdObjUnwrapped.autocommit = false;

            // Statement Id is required on all transaction operations.
            cmdObjUnwrapped.stmtId = new NumberInt(_nextStatementId);

            // 'readConcern' and 'startTransaction' can only be specified on the first statement in
            // a transaction.
            if (_nextStatementId == 0) {
                cmdObjUnwrapped.startTransaction = true;
                if (_txnOptions.getTxnReadConcern() !== undefined) {
                    // Override the readConcern with the one specified during startTransaction.
                    cmdObjUnwrapped.readConcern = _txnOptions.getTxnReadConcern();
                }
            }

            // Reserve the statement ids for batch writes.
            switch (cmdName) {
                case "insert":
                    _nextStatementId += cmdObjUnwrapped.documents.length;
                    break;
                case "update":
                    _nextStatementId += cmdObjUnwrapped.updates.length;
                    break;
                case "delete":
                    _nextStatementId += cmdObjUnwrapped.deletes.length;
                    break;
                default:
                    _nextStatementId += 1;
            }

            return cmdObj;
        };

        this.startTransaction = function startTransaction(txnOptsObj) {
            // If the session is already in a transaction, raise an error.
            if (this.isInActiveTransaction()) {
                throw new Error("Transaction already in progress on this session.");
            }
            _txnOptions = new TransactionOptions(txnOptsObj);
            _txnState = ServerSession.TransactionStates.kActive;
            _nextStatementId = 0;
            _txnNumber++;
        };

        this.commitTransaction = function commitTransaction(driverSession) {
            // If the session has no active transaction, raise an error.
            if (!this.isInActiveTransaction()) {
                throw new Error("There is no active transaction to commit on this session.");
            }
            // run commitTxn command
            return endTransaction("commitTransaction", driverSession);
        };

        this.abortTransaction = function abortTransaction(driverSession) {
            // If the session has no active transaction, raise an error.
            if (!this.isInActiveTransaction()) {
                throw new Error("There is no active transaction to abort on this session.");
            }
            // run abortTxn command
            return endTransaction("abortTransaction", driverSession);
        };

        const endTransaction = (commandName, driverSession) => {
            // If commitTransaction or abortTransaction is the first statement in a
            // transaction, it should not send a command to the server and should mark the
            // transaction as inactive.
            if (this.isFirstStatement()) {
                _txnState = ServerSession.TransactionStates.kInactive;
                return {"ok": 1};
            }

            let cmd = {[commandName]: 1, txnNumber: NumberLong(_txnNumber)};
            // writeConcern should only be specified on commit or abort. If a writeConcern is
            // not specified from the default transaction options, it will be inherited from
            // the session.
            if (this.client.getWriteConcern(driverSession) !== undefined) {
                cmd.writeConcern = this.client.getWriteConcern(driverSession);
            }
            if (_txnOptions.getTxnWriteConcern() !== undefined) {
                cmd.writeConcern = _txnOptions.getTxnWriteConcern();
            }

            // If commit or abort raises an error, the transaction's state should still change
            // to inactive.
            let res;
            try {
                // run command against the admin database.
                res = this.client.runCommand(driverSession, "admin", cmd, 0);
            } finally {
                _txnState = ServerSession.TransactionStates.kInactive;
            }
            return res;
        };
    }

    // TransactionStates represents the state of the current transaction. The default state
    // is `inactive` until startTransaction is called and changes the state to `active`.
    // Calling a successful abort or commitTransaction will change the state to `inactive`.
    ServerSession.TransactionStates = {
        kActive: 'active',
        kInactive: 'inactive',
    };

    function makeDriverSessionConstructor(implMethods, defaultOptions = {}) {
        return function(client, options = defaultOptions) {
            let _options = options;
            let _hasEnded = false;

            let _operationTime;
            let _clusterTime;

            if (!(_options instanceof SessionOptions)) {
                _options = new SessionOptions(_options);
            }

            this._serverSession = implMethods.createServerSession(client);

            this.getClient = function getClient() {
                return client;
            };

            this._getSessionAwareClient = function _getSessionAwareClient() {
                return this._serverSession.client;
            };

            this.getOptions = function getOptions() {
                return _options;
            };

            this.getSessionId = function getSessionId() {
                if (!this._serverSession.hasOwnProperty("handle")) {
                    return null;
                }
                return this._serverSession.handle.getId();
            };

            this.getOperationTime = function getOperationTime() {
                return _operationTime;
            };

            this.advanceOperationTime = function advanceOperationTime(operationTime) {
                if (!isNonNullObject(_operationTime) ||
                    bsonWoCompare({_: operationTime}, {_: _operationTime}) > 0) {
                    _operationTime = operationTime;
                }
            };

            this.resetOperationTime_forTesting = function resetOperationTime_forTesting() {
                _operationTime = undefined;
            };

            this.getClusterTime = function getClusterTime() {
                return _clusterTime;
            };

            this.advanceClusterTime = function advanceClusterTime(clusterTime) {
                if (!isNonNullObject(_clusterTime) ||
                    bsonWoCompare({_: clusterTime.clusterTime}, {_: _clusterTime.clusterTime}) >
                        0) {
                    _clusterTime = clusterTime;
                }
            };

            this.resetClusterTime_forTesting = function resetClusterTime_forTesting() {
                _clusterTime = undefined;
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

            this.shellPrint = function() {
                return this.toString();
            };

            this.tojson = function _tojson(...args) {
                return tojson(this.getSessionId(), ...args);
            };

            this.toString = function toString() {
                const sessionId = this.getSessionId();
                if (sessionId === null) {
                    return "dummy session";
                }
                return "session " + tojson(sessionId);
            };

            this.startTransaction = function startTransaction(txnOptsObj = {}) {
                this._serverSession.startTransaction(txnOptsObj);
            };

            this.commitTransaction = function commitTransaction() {
                assert.commandWorked(this._serverSession.commitTransaction(this));
            };

            this.abortTransaction = function abortTransaction() {
                // Intentionally ignore command result.
                this._serverSession.abortTransaction(this);
            };
            this.commitTransaction_forTesting = function commitTransaction_forTesting() {
                return this._serverSession.commitTransaction(this);
            };

            this.abortTransaction_forTesting = function abortTransaction_forTesting() {
                return this._serverSession.abortTransaction(this);
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

    function DelegatingDriverSession(client, originalSession) {
        const sessionAwareClient = new SessionAwareClient(client);

        this.getClient = function() {
            return client;
        };

        this._getSessionAwareClient = function() {
            return sessionAwareClient;
        };

        this.getDatabase = function(dbName) {
            const db = client.getDB(dbName);
            db._session = this;
            return db;
        };

        return new Proxy(this, {
            get: function get(target, property, receiver) {
                // If the property is defined on the DelegatingDriverSession instance itself, then
                // return it. Otherwise, get the value of the property from the `originalSession`
                // instance.
                if (target.hasOwnProperty(property)) {
                    return target[property];
                }
                return originalSession[property];
            },
        });
    }

    // The default session on the Mongo connection object should report that causal consistency
    // isn't enabled when interrogating the SessionOptions since it must be enabled on the Mongo
    // connection object.
    //
    // The default session on the Mongo connection object should also report that retryable
    // writes isn't enabled when interrogating the SessionOptions since `DummyDriverSession` won't
    // ever assign a transaction number.
    const DummyDriverSession =
        makeDriverSessionConstructor(  // Force clang-format to break this line.
            {
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

                      assignTxnInfo: function assignTxnInfo(cmdObj) {
                          return cmdObj;
                      },

                      isInActiveTransaction: function isInActiveTransaction() {
                          return false;
                      },

                      isFirstStatement: function isFirstStatement() {
                          return false;
                      },

                      getTxnOptions: function getTxnOptions() {
                          return {};
                      },

                      startTransaction: function startTransaction() {
                          throw new Error("Must call startSession() on the Mongo connection " +
                                          "object before starting a transaction.");
                      },

                      commitTransaction: function commitTransaction() {
                          throw new Error("Must call startSession() on the Mongo connection " +
                                          "object before committing a transaction.");
                      },

                      abortTransaction: function abortTransaction() {
                          throw new Error("Must call startSession() on the Mongo connection " +
                                          "object before aborting a transaction.");
                      },
                  };
              },

              endSession: function endSession(serverSession) {},
            },
            {causalConsistency: false, retryWrites: false});

    // We don't actually put anything on DriverSession.prototype, but this way
    // `session instanceof DriverSession` will work for DummyDriverSession instances.
    DummyDriverSession.prototype = Object.create(DriverSession.prototype);
    DummyDriverSession.prototype.constructor = DriverSession;

    return {
        DriverSession: DriverSession,
        SessionOptions: SessionOptions,
        _DummyDriverSession: DummyDriverSession,
        _DelegatingDriverSession: DelegatingDriverSession,
    };
})();
