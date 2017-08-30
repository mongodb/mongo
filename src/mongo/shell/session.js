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
    }

    function SessionAwareClient(client) {
        const kWireVersionSupportingLogicalSession = 6;

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

        function prepareCommandRequest(driverSession, cmdObj) {
            if (serverSupports(kWireVersionSupportingLogicalSession)) {
                cmdObj = driverSession._serverSession.injectSessionId(cmdObj);
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

        this.runCommand = function runCommand(driverSession, dbName, cmdObj, options) {
            cmdObj = prepareCommandRequest(driverSession, cmdObj);

            const res = client.runCommand(dbName, cmdObj, options);
            processCommandResponse(driverSession, res);

            return res;
        };

        this.runCommandWithMetadata = function runCommandWithMetadata(
            driverSession, dbName, metadata, cmdObj) {
            cmdObj = prepareCommandRequest(driverSession, cmdObj);

            const res = client.runCommandWithMetadata(dbName, metadata, cmdObj);
            processCommandResponse(driverSession, res);

            return res;
        };
    }

    function ServerSession(client) {
        let _lastUsed = new Date();

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
    }

    function makeDriverSessionConstructor(implMethods) {
        return function(client, options = {}) {
            let _options = options;
            let _hasEnded = false;

            if (!(_options instanceof SessionOptions)) {
                _options = new SessionOptions(_options);
            }

            this._serverSession = implMethods.createServerSession(client);
            this._operationTime = null;

            this.getClient = function getClient() {
                return client;
            };

            this.getOptions = function getOptions() {
                return _options;
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
