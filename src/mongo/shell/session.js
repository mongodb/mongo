/**
 * Implements the sessions api for the shell.
 */
var _session_api_module = (function() {
    "use strict";

    var ServerSession = function(client) {
        this._handle = client._startSession(this._id);
        this._lastUsed = new Date();
        this._id = this._handle.getId();
    };

    ServerSession.prototype.updateLastUsed = function() {
        this._lastUsed = new Date();
    };

    var DriverSession = function(client, opts) {
        this._serverSession = new ServerSession(client);
        this._client = client;
        this._options = opts;
        this._hasEnded = false;
    };

    DriverSession.prototype.getClient = function() {
        return this._client;
    };

    DriverSession.prototype.getOptions = function() {
        return this._options;
    };

    DriverSession.prototype.hasEnded = function() {
        return this._hasEnded;
    };

    DriverSession.prototype.endSession = function() {
        if (!this._hasEnded) {
            this._hasEnded = true;
            this._serverSession._handle.end();
        }
    };

    DriverSession.prototype.getDatabase = function(db) {
        var db = new DB(this._client, db);
        db._session = this;
        return db;
    };

    var SessionOptions = function() {};

    SessionOptions.prototype.getReadPreference = function() {
        return this._readPreference;
    };

    SessionOptions.prototype.setReadPreference = function(readPreference) {
        this._readPreference = readPreference;
    };

    SessionOptions.prototype.getReadConcern = function() {
        return this._readConcern;
    };

    SessionOptions.prototype.setReadConcern = function(readConcern) {
        this._readConcern = readConcern;
    };

    SessionOptions.prototype.getWriteConcern = function() {
        return this._writeConcern;
    };

    SessionOptions.prototype.setWriteConcern = function(writeConcern) {
        this._writeConcern = writeConcern;
    };

    var module = {};

    module.DriverSession = DriverSession;
    module.SessionOptions = SessionOptions;

    return module;
})();

// Globals
DriverSession = _session_api_module.DriverSession;
SessionOptions = _session_api_module.SessionOptions;
