/**
 * Implements the sessions api for the shell.
 *
 * Roughly follows the driver sessions spec:
 * https://github.com/mongodb/specifications/blob/master/source/sessions/driver-sessions.rst#abstract
 */
var {
    DriverSession,
    SessionOptions,
    _DummyDriverSession,
    _DelegatingDriverSession,
    _ServerSession,
} = (function() {
    "use strict";

    const kShellDefaultShouldRetryWrites =
        typeof _shouldRetryWrites === "function" ? _shouldRetryWrites() : false;

    function isNonNullObject(obj) {
        return typeof obj === "object" && obj !== null;
    }

    function isAcknowledged(cmdObj) {
        if (isNonNullObject(cmdObj.writeConcern)) {
            const writeConcern = cmdObj.writeConcern;
            // Intentional use of "==" comparing NumberInt, NumberLong, or plain Number.
            if (writeConcern.hasOwnProperty("w") && writeConcern.w == 0) {
                return false;
            }
        }

        return true;
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

    const kWireVersionSupportingCausalConsistency = 6;
    const kWireVersionSupportingLogicalSession = 6;
    const kWireVersionSupportingRetryableWrites = 6;
    const kWireVersionSupportingMultiDocumentTransactions = 7;

    function processCommandResponse(driverSession, client, res) {
        if (res.hasOwnProperty("operationTime")) {
            driverSession.advanceOperationTime(res.operationTime);
        }

        if (res.hasOwnProperty("$clusterTime")) {
            driverSession.advanceClusterTime(res.$clusterTime);
            client.advanceClusterTime(res.$clusterTime);
        }
    }

    function SessionAwareClient(client) {
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

        // TODO: Update this allowlist, or convert it to a denylist depending on the outcome of
        // SERVER-31743.
        const kCommandsThatSupportReadConcern = new Set([
            "aggregate",
            "count",
            "distinct",
            "find",
            "explain",
            "geoNear",
            "group",
        ]);

        this.canUseReadConcern = function(driverSession, cmdObj) {
            // Always attach the readConcern to the first statement of the transaction, whether it
            // is a read or a write.
            if (driverSession._serverSession.isTxnActive()) {
                return driverSession._serverSession.isFirstStatement();
            }

            let cmdName = Object.keys(cmdObj)[0];

            if (!kCommandsThatSupportReadConcern.has(cmdName)) {
                return false;
            }

            if (cmdName === "explain") {
                return kCommandsThatSupportReadConcern.has(Object.keys(cmdObj.explain)[0]);
            }

            return true;
        };

        function gossipClusterTime(cmdObj, clusterTime) {
            cmdObj = Object.assign({}, cmdObj);

            const cmdName = Object.keys(cmdObj)[0];

            if (!cmdObj.hasOwnProperty("$clusterTime")) {
                cmdObj.$clusterTime = clusterTime;
            }

            return cmdObj;
        }

        function establishSessionReadConcern(cmdObj, driverSession) {
            // `driverSession.getOperationTime()` is the smallest time needed for performing a
            // causally consistent read using the current session. Note that
            // `client.getClusterTime()` is no smaller than the operation time and would
            // therefore only be less efficient to wait until.
            const operationTime = driverSession.getOperationTime();
            if (operationTime === undefined) {
                return cmdObj;
            }

            cmdObj = Object.assign({}, cmdObj);
            let cmdName = Object.keys(cmdObj)[0];

            // Explain read concerns are on the inner command.
            let cmdObjUnwrapped = cmdObj;
            if (cmdName === "explain") {
                cmdObj[cmdName] = Object.assign({}, cmdObj[cmdName]);
                cmdObjUnwrapped = cmdObj[cmdName];
            }

            // Transaction read concerns are handled later in assignTxnInfo().
            const sessionReadConcern = driverSession.getOptions().getReadConcern();
            cmdObjUnwrapped.readConcern =
                Object.assign({}, sessionReadConcern, cmdObjUnwrapped.readConcern);
            const readConcern = cmdObjUnwrapped.readConcern;

            if (!readConcern.hasOwnProperty("afterClusterTime")) {
                readConcern.afterClusterTime = operationTime;
            }

            return cmdObj;
        }

        this.prepareCommandRequest = function(driverSession, cmdObj) {
            if (driverSession._isExplicit && !isAcknowledged(cmdObj)) {
                throw new Error("Unacknowledged writes are prohibited with sessions");
            }

            if (serverSupports(kWireVersionSupportingLogicalSession) &&
                // Always attach sessionId from explicit sessions.
                (driverSession._isExplicit ||
                 // Check that implicit sessions are not disabled.
                 !jsTest.options().disableImplicitSessions)) {
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

            if (serverSupports(kWireVersionSupportingCausalConsistency) &&
                (client.isReplicaSetMember() || client.isMongos()) &&
                (driverSession.getOptions().isCausalConsistency() ||
                 client.isCausalConsistency()) &&
                this.canUseReadConcern(driverSession, cmdObj)) {
                cmdObj = establishSessionReadConcern(cmdObj, driverSession);
            }

            // All commands go through transaction code, which will determine if the command is a
            // part of the current transaction and will assign transaction info accordingly.
            cmdObj = driverSession._serverSession.assignTxnInfo(cmdObj);

            // Retryable writes code should execute only we are not in an active transaction.
            if ((jsTest.options().alwaysInjectTransactionNumber ||
                 (Object.keys(cmdObj)[0] == "testInternalTransactions")) &&
                serverSupports(kWireVersionSupportingRetryableWrites) &&
                driverSession.getOptions().shouldRetryWrites() &&
                _ServerSession.canRetryWrites(cmdObj)) {
                cmdObj = driverSession._serverSession.assignTransactionNumber(cmdObj);
            }

            return cmdObj;
        };

        /**
         * Returns true if the error code is retryable, assuming the command is idempotent.
         *
         * The Retryable Writes specification defines a RetryableError as any network error,
         * any of the following error codes, or an error response with a different code
         * containing the phrase "not master" or "node is recovering".
         *
         * https://github.com/mongodb/specifications/blob/5b53e0baca18ba111364d479a37fa9195ef801a6/
         * source/retryable-writes/retryable-writes.rst#terms
         */
        function isRetryableCode(code) {
            return ErrorCodes.isNetworkError(code) || ErrorCodes.isNotPrimaryError(code) ||
                ErrorCodes.isShutdownError(code) || ErrorCodes.WriteConcernFailed === code;
        }

        function runClientFunctionWithRetries(
            driverSession, cmdObj, clientFunction, clientFunctionArguments) {
            let cmdName = Object.keys(cmdObj)[0];

            // TODO SERVER-33921: Revisit how the mongo shell decides whether it should
            // retry a command or not.
            const sessionOptions = driverSession.getOptions();
            let numRetries =
                (sessionOptions.shouldRetryWrites() && cmdObj.hasOwnProperty("txnNumber") &&
                 !jsTest.options().skipRetryOnNetworkError &&
                 !driverSession._serverSession.isTxnActive())
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

                    // We run a "hello" command explicitly to force the underlying
                    // DBClient to reconnect to the server.
                    const res = client.getDB('admin')._helloOrLegacyHello();
                    if (res.ok !== 1) {
                        throw e;
                    }

                    // It's possible that the server we're connected with after
                    // re-establishing our connection doesn't support retryable writes. If
                    // that happens, then we just return the original network error back to
                    // the user.
                    const serverSupportsRetryableWrites = res.hasOwnProperty("minWireVersion") &&
                        res.hasOwnProperty("maxWireVersion") &&
                        res.minWireVersion <= kWireVersionSupportingRetryableWrites &&
                        kWireVersionSupportingRetryableWrites <= res.maxWireVersion;

                    if (!serverSupportsRetryableWrites) {
                        throw e;
                    }
                }

                // Handle ErrorCodes.Reauthentication first.
                if (res !== undefined && res.code === ErrorCodes.ReauthenticationRequired) {
                    try {
                        const accessToken = client._refreshAccessToken();
                        assert(client.getDB('$external').auth({
                            oidcAccessToken: accessToken,
                            mechanism: 'MONGODB-OIDC'
                        }));
                        continue;
                    } catch (e) {
                        // Could not automatically reauthenticate, return the error response
                        // as-is.
                        jsTest.log('Assertion thrown when performing refresh flow: ' + e);
                        return res;
                    }
                }

                if (numRetries > 0) {
                    --numRetries;

                    if (res === undefined) {
                        if (jsTest.options().logRetryAttempts) {
                            jsTest.log("Retrying " + cmdName +
                                       " with original command request: " + tojson(cmdObj) +
                                       " due to network error, subsequent retries remaining: " +
                                       numRetries);
                        }
                        continue;
                    }

                    if (isRetryableCode(res.code)) {
                        if (jsTest.options().logRetryAttempts) {
                            jsTest.log("Retrying " + cmdName +
                                       " with original command request: " + tojson(cmdObj) +
                                       " due to retryable error (code=" + res.code +
                                       "), subsequent retries remaining: " + numRetries);
                        }
                        if (client.isReplicaSetConnection()) {
                            client._markNodeAsFailed(res._mongo.host, res.code, res.errmsg);
                        }
                        continue;
                    }

                    if (Array.isArray(res.writeErrors)) {
                        // If any of the write operations in the batch fails with a
                        // retryable error, then we retry the entire batch.
                        const writeError =
                            res.writeErrors.find((writeError) => isRetryableCode(writeError.code));

                        if (writeError !== undefined) {
                            if (jsTest.options().logRetryAttempts) {
                                jsTest.log(
                                    "Retrying " + cmdName +
                                    " with original command request: " + tojson(cmdObj) +
                                    " due to retryable write error (code=" + writeError.code +
                                    "), subsequent retries remaining: " + numRetries);
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
                                       " with original command request: " + tojson(cmdObj) +
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
            cmdObj = this.prepareCommandRequest(driverSession, cmdObj);

            const res = runClientFunctionWithRetries(
                driverSession, cmdObj, client.runCommand, [dbName, cmdObj, options]);

            processCommandResponse(driverSession, client, res);
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
        let _txnOptions;

        // Keep track of the next available statement id of a transaction.
        let _nextStatementId = 0;
        let _lastUsed = new Date();

        if (!serverSupports(kWireVersionSupportingLogicalSession)) {
            throw new DriverSession.UnsupportedError(
                "Logical Sessions are only supported on server versions 3.6 and greater.");
        }
        this.handle = client._startSession();

        function serverSupports(wireVersion) {
            return client.getMinWireVersion() <= wireVersion &&
                wireVersion <= client.getMaxWireVersion();
        }

        const hasTxnState = ((name) => this.handle.getTxnState() === name);
        const setTxnState = ((name) => this.handle.setTxnState(name));

        this.isTxnActive = function isTxnActive() {
            return hasTxnState("active");
        };

        this.isFirstStatement = function isFirstStatement() {
            return _nextStatementId === 0;
        };

        this.getLastUsed = function getLastUsed() {
            return _lastUsed;
        };

        this.getTxnNumber = function getTxnNumber() {
            return this.handle.getTxnNumber();
        };

        this.setTxnNumber_forTesting = function setTxnNumber_forTesting(newTxnNumber) {
            this.handle.setTxnNumber(newTxnNumber);
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

            if (!cmdObj.hasOwnProperty("lsid")) {
                if (isAcknowledged(cmdObj)) {
                    cmdObj.lsid = this.handle.getId();
                }

                // We consider the session to still be in use by the client any time the session id
                // is injected into the command object as part of making a request.
                updateLastUsed();
            }

            return cmdObj;
        };

        this.assignTransactionNumber = function assignTransactionNumber(cmdObj) {
            cmdObj = Object.assign({}, cmdObj);

            const cmdName = Object.keys(cmdObj)[0];

            if (!cmdObj.hasOwnProperty("txnNumber")) {
                this.handle.incrementTxnNumber();
                cmdObj.txnNumber = this.handle.getTxnNumber();
            }

            return cmdObj;
        };

        this.assignTxnInfo = function assignTxnInfo(cmdObj) {
            // We will want to reset the transaction state to 'inactive' if a normal operation
            // follows a committed or aborted transaction.
            if ((hasTxnState("aborted")) ||
                (hasTxnState("committed") && Object.keys(cmdObj)[0] !== "commitTransaction")) {
                setTxnState("inactive");
            }

            // If we're not in an active transaction or performing a retry on commitTransaction,
            // return early.
            if (hasTxnState("inactive")) {
                return cmdObj;
            }

            // If we reconnect to a 3.6 server in the middle of a transaction, we
            // catch it here.
            if (!serverSupports(kWireVersionSupportingMultiDocumentTransactions)) {
                setTxnState("inactive");
                throw new Error(
                    "Transactions are only supported on server versions 4.0 and greater.");
            }
            cmdObj = Object.assign({}, cmdObj);

            const cmdName = Object.keys(cmdObj)[0];

            if (!cmdObj.hasOwnProperty("txnNumber")) {
                cmdObj.txnNumber = this.handle.getTxnNumber();
            }

            // All operations of a multi-statement transaction must specify autocommit=false.
            cmdObj.autocommit = false;

            // Statement Id is required on all transaction operations.
            cmdObj.stmtId = new NumberInt(_nextStatementId);

            // 'readConcern' and 'startTransaction' can only be specified on the first statement
            // in a transaction.
            if (_nextStatementId == 0) {
                cmdObj.startTransaction = true;
                if (_txnOptions.getTxnReadConcern() !== undefined) {
                    // Override the readConcern with the one specified during startTransaction.
                    cmdObj.readConcern =
                        Object.assign({}, cmdObj.readConcern, _txnOptions.getTxnReadConcern());
                }
            }

            // Reserve the statement ids for batch writes.
            switch (cmdName) {
                case "insert":
                    _nextStatementId += cmdObj.documents.length;
                    break;
                case "update":
                    _nextStatementId += cmdObj.updates.length;
                    break;
                case "delete":
                    _nextStatementId += cmdObj.deletes.length;
                    break;
                default:
                    _nextStatementId += 1;
            }

            return cmdObj;
        };

        this.startTransaction = function startTransaction(txnOptsObj, ignoreActiveTxn) {
            // If the session is already in a transaction, raise an error. If ignoreActiveTxn
            // is true, don't raise an error. This is to allow multiple threads to try to
            // use the same session in a concurrency workload.
            if (this.isTxnActive() && !ignoreActiveTxn) {
                throw new Error("Transaction already in progress on this session.");
            }
            if (!serverSupports(kWireVersionSupportingMultiDocumentTransactions)) {
                throw new Error(
                    "Transactions are only supported on server versions 4.0 and greater.");
            }
            _txnOptions = new TransactionOptions(txnOptsObj);
            setTxnState("active");
            _nextStatementId = 0;
            this.handle.incrementTxnNumber();
        };

        this.commitTransaction = function commitTransaction(driverSession) {
            // If the transaction state is already 'aborted' we cannot try to commit it.
            if (hasTxnState("aborted")) {
                throw new Error("Cannot call commitTransaction after calling abortTransaction.");
            }
            // If the session has no active transaction, raise an error.
            if (hasTxnState("inactive")) {
                throw new Error("There is no active transaction to commit on this session.");
            }
            // run commitTxn command
            return endTransaction("commitTransaction", driverSession);
        };

        this.abortTransaction = function abortTransaction(driverSession) {
            // If the transaction state is already 'aborted' we cannot try to abort it again.
            if (hasTxnState("aborted")) {
                throw new Error("Cannot call abortTransaction twice.");
            }
            // We cannot attempt to abort a transaction that has already been committed.
            if (hasTxnState("committed")) {
                throw new Error("Cannot call abortTransaction after calling commitTransaction.");
            }
            // If the session has no active transaction, raise an error.
            if (hasTxnState("inactive")) {
                throw new Error("There is no active transaction to abort on this session.");
            }
            // run abortTxn command
            return endTransaction("abortTransaction", driverSession);
        };

        this.getTxnWriteConcern = function getTxnWriteConcern(driverSession) {
            // If a writeConcern is not specified from the default transaction options, it will be
            // inherited from the session.
            let writeConcern = undefined;
            const sessionAwareClient = driverSession._getSessionAwareClient();
            if (sessionAwareClient.getWriteConcern(driverSession) !== undefined) {
                writeConcern = sessionAwareClient.getWriteConcern(driverSession);
            }
            if (_txnOptions.getTxnWriteConcern() !== undefined) {
                writeConcern = _txnOptions.getTxnWriteConcern();
            }
            return writeConcern;
        };

        const endTransaction = (commandName, driverSession) => {
            // If commitTransaction or abortTransaction is the first statement in a
            // transaction, it should not send a command to the server and should mark the
            // transaction as 'committed' or 'aborted' accordingly.
            if (this.isFirstStatement()) {
                if (commandName === "commitTransaction") {
                    setTxnState("committed");
                } else {
                    setTxnState("aborted");
                }
                return {"ok": 1};
            }

            let cmd = {[commandName]: 1, txnNumber: this.handle.getTxnNumber()};
            // writeConcern should only be specified on commit or abort.
            const writeConcern = driverSession._serverSession.getTxnWriteConcern(driverSession);
            if (writeConcern !== undefined) {
                cmd.writeConcern = writeConcern;
            }

            // If commit or abort raises an error, the transaction's state should still change.
            let res;
            try {
                // run command against the admin database.
                res = driverSession._getSessionAwareClient().runCommand(
                    driverSession, "admin", cmd, 0);
            } finally {
                if (commandName === "commitTransaction") {
                    setTxnState("committed");
                } else {
                    setTxnState("aborted");
                }
            }
            return res;
        };
    }

    ServerSession.canRetryWrites = function canRetryWrites(cmdObj) {
        let cmdName = Object.keys(cmdObj)[0];

        if (cmdObj.hasOwnProperty("autocommit")) {
            return false;
        }

        if (!isAcknowledged(cmdObj)) {
            return false;
        }

        if (cmdName == "testInternalTransactions") {
            return true;
        }

        if (cmdName === "insert") {
            if (!Array.isArray(cmdObj.documents)) {
                // The command object is malformed, so we'll just leave it as-is and let the server
                // reject it.
                return false;
            }

            // Both single-statement operations (e.g. insertOne()) and multi-statement operations
            // (e.g. insertMany()) can be retried regardless of whether they are executed in order
            // by the server.
            return true;
        } else if (cmdName === "update") {
            if (!Array.isArray(cmdObj.updates)) {
                // The command object is malformed, so we'll just leave it as-is and let the server
                // reject it.
                return false;
            }

            const hasMultiUpdate = cmdObj.updates.some(updateOp => updateOp.multi);
            if (hasMultiUpdate) {
                // Operations that modify multiple documents (e.g. updateMany()) cannot be retried.
                return false;
            }

            // Both single-statement operations (e.g. updateOne()) and multi-statement operations
            // (e.g. bulkWrite()) can be retried regardless of whether they are executed in order by
            // the server.
            return true;
        } else if (cmdName === "delete") {
            if (!Array.isArray(cmdObj.deletes)) {
                // The command object is malformed, so we'll just leave it as-is and let the server
                // reject it.
                return false;
            }

            // We use bsonWoCompare() in order to handle cases where the limit is specified as a
            // NumberInt() or NumberLong() instance.
            const hasMultiDelete =
                cmdObj.deletes.some(deleteOp => bsonWoCompare({_: deleteOp.limit}, {_: 0}) === 0);
            if (hasMultiDelete) {
                // Operations that modify multiple documents (e.g. deleteMany()) cannot be retried.
                return false;
            }

            // Both single-statement operations (e.g. deleteOne()) and multi-statement operations
            // (e.g. bulkWrite()) can be retried regardless of whether they are executed in order by
            // the server.
            return true;
        } else if (cmdName === "findAndModify" || cmdName === "findandmodify") {
            // Operations that modify a single document (e.g. findOneAndUpdate()) can be retried.
            return true;
        } else if (cmdName === "bulkWrite") {
            return true;
        }

        return false;
    };

    function makeDriverSessionConstructor(implMethods, defaultOptions = {}) {
        var driverSessionConstructor = function(client, options = defaultOptions) {
            const sessionAwareClient = new SessionAwareClient(client);

            let _options = options;
            let _hasEnded = false;

            let _operationTime;
            let _clusterTime;

            if (!(_options instanceof SessionOptions)) {
                _options = new SessionOptions(_options);
            }

            this._serverSession = implMethods.createServerSession(client);

            this._isExplicit = true;

            this.getClient = function getClient() {
                return client;
            };

            this._getSessionAwareClient = function _getSessionAwareClient() {
                return sessionAwareClient;
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

            this.getTxnNumber_forTesting = function getTxnNumber_forTesting() {
                return this._serverSession.getTxnNumber();
            };

            this.getTxnWriteConcern_forTesting = function getTxnWriteConcern_forTesting() {
                return this._serverSession.getTxnWriteConcern(this);
            };

            this.setTxnNumber_forTesting = function setTxnNumber_forTesting(newTxnNumber) {
                this._serverSession.setTxnNumber_forTesting(newTxnNumber);
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

            this.startTransaction_forTesting = function startTransaction_forTesting(
                txnOptsObj = {}, {ignoreActiveTxn: ignoreActiveTxn = false} = {}) {
                this._serverSession.startTransaction(txnOptsObj, ignoreActiveTxn);
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

            this.processCommandResponse_forTesting = function processCommandResponse_forTesting(
                res) {
                processCommandResponse(this, client, res);
            };
        };

        // Having a specific Error for when logical sessions aren't supported by the server, allows
        // the correct fallback behavior in this case (while propagating other errors).
        driverSessionConstructor.UnsupportedError = function(message) {
            this.name = "DriverSession.UnsupportedError";
            this.message = message;
            this.stack = this.toString() + "\n" + (new Error()).stack;
        };
        driverSessionConstructor.UnsupportedError.prototype = Object.create(Error.prototype);
        driverSessionConstructor.UnsupportedError.prototype.constructor =
            driverSessionConstructor.UnsupportedError;

        return driverSessionConstructor;
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

                        isTxnActive: function isTxnActive() {
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
        _ServerSession: ServerSession,
    };
})();
