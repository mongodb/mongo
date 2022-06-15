__quiet = false;
__magicNoPrint = {
    __magicNoPrint: 1111
};
_verboseShell = false;

chatty = function(s) {
    if (!__quiet)
        print(s);
};

function reconnect(db) {
    assert.soon(function() {
        try {
            db.runCommand({ping: 1});
            return true;
        } catch (x) {
            return false;
        }
    });
}

function _getErrorWithCode(codeOrObj, message) {
    var e = new Error(message);
    if (typeof codeOrObj === "object" && codeOrObj !== null) {
        if (codeOrObj.hasOwnProperty("code")) {
            e.code = codeOrObj.code;
        }

        if (codeOrObj.hasOwnProperty("writeErrors")) {
            e.writeErrors = codeOrObj.writeErrors;
        }

        if (codeOrObj.hasOwnProperty("errorLabels")) {
            e.errorLabels = codeOrObj.errorLabels;
        }
    } else if (typeof codeOrObj === "number") {
        e.code = codeOrObj;
    }

    return e;
}

/**
 * Executes the specified function and retries it if it fails due to exception related to network
 * error. If it exhausts the number of allowed retries, it simply throws the last exception.
 *
 * Returns the return value of the input call.
 */
function retryOnNetworkError(func, numRetries, sleepMs) {
    numRetries = numRetries || 1;
    sleepMs = sleepMs || 1000;

    while (true) {
        try {
            return func();
        } catch (e) {
            if (isNetworkError(e) && numRetries > 0) {
                print("Network error occurred and the call will be retried: " +
                      tojson({error: e.toString(), stack: e.stack}));
                numRetries--;
                sleep(sleepMs);
            } else {
                throw e;
            }
        }
    }
}

const shellGeneratedNetworkErrs = ["network error", "error doing query", "socket exception"];

const networkErrs = ["SocketException", "HostNotFound"];
const networkErrsPlusShellGeneratedNetworkErrs = [...networkErrs, ...shellGeneratedNetworkErrs];
/**
 * Determine if a provided object represents a network error
 * @param {object|Error|string|number} errorOrResponse A command response, error or scalar
 *     representing an error
 */
function isNetworkError(errorOrResponse) {
    // First check if this is a command response, if so check by error code
    if (errorOrResponse.code) {
        if (ErrorCodes.isNetworkError(errorOrResponse.code)) {
            return true;
        }
    }

    // Then check if it's an Error, if so see if any of the known network error strings appear
    // in the given message.
    if (errorOrResponse.message) {
        if (networkErrsPlusShellGeneratedNetworkErrs.some(
                err => errorOrResponse.message.includes(err))) {
            return true;
        }
    }

    // Otherwise fall back to checking by scalar value
    return ErrorCodes.isNetworkError(errorOrResponse);
}

const retryableErrs = [
    "Interrupted",
    "InterruptedAtShutdown",
    "InterruptedDueToReplStateChange",
    "ExceededTimeLimit",
    "MaxTimeMSExpired",
    "CursorKilled",
    "LockTimeout",
    "ShutdownInProgress",
    "HostUnreachable",
    "HostNotFound",
    "NetworkTimeout",
    "SocketException",
    "NotWritablePrimary",
    "NotPrimaryNoSecondaryOk",
    "NotPrimaryOrSecondary",
    "PrimarySteppedDown",
    "WriteConcernFailed",
    "WriteConcernLegacyOK",
    "UnknownReplWriteConcern",
    "UnsatisfiableWriteConcern"
];
const retryableErrsPlusShellGeneratedNetworkErrs = [...retryableErrs, ...shellGeneratedNetworkErrs];
/**
 * Determine if a provided object represents a retryable error
 * @param {object|Error|string|number} errorOrResponse A command response, error or scalar
 *     representing an error
 */
function isRetryableError(errorOrResponse) {
    // First check if this is a command response, if so determine retryability by error code
    if (errorOrResponse.code) {
        if (ErrorCodes.isRetriableError(errorOrResponse.code)) {
            return true;
        }
    }

    // Then check if it's an Error, if so determine retryability by checking the error message
    if (errorOrResponse.message) {
        // See if any of the known network error strings appear in the given message.
        if (retryableErrsPlusShellGeneratedNetworkErrs.some(
                err => errorOrResponse.message.includes(err))) {
            return true;
        }
    }

    // Otherwise fall back to checking by scalar value
    return ErrorCodes.isRetriableError(errorOrResponse);
}

// Please consider using bsonWoCompare instead of this as much as possible.
friendlyEqual = function(a, b) {
    if (a == b)
        return true;

    a = tojson(a, false, true);
    b = tojson(b, false, true);

    if (a == b)
        return true;

    var clean = function(s) {
        s = s.replace(/NumberInt\((\-?\d+)\)/g, "$1");
        return s;
    };

    a = clean(a);
    b = clean(b);

    if (a == b)
        return true;

    return false;
};

printStackTrace = function() {
    try {
        throw new Error("Printing Stack Trace");
    } catch (e) {
        print(e.stack);
    }
};

/**
 * <p> Set the shell verbosity. If verbose the shell will display more information about command
 * results. </>
 * <p> Default is off. <p>
 * @param {Bool} verbosity on / off
 */
setVerboseShell = function(value) {
    if (value == undefined)
        value = true;
    _verboseShell = value;
};

// Formats a simple stacked horizontal histogram bar in the shell.
// @param data array of the form [[ratio, symbol], ...] where ratio is between 0 and 1 and
//             symbol is a string of length 1
// @param width width of the bar (excluding the left and right delimiters [ ] )
// e.g. _barFormat([[.3, "="], [.5, '-']], 80) returns
//      "[========================----------------------------------------                ]"
_barFormat = function(data, width) {
    var remaining = width;
    var res = "[";
    for (var i = 0; i < data.length; i++) {
        for (var x = 0; x < data[i][0] * width; x++) {
            if (remaining-- > 0) {
                res += data[i][1];
            }
        }
    }
    while (remaining-- > 0) {
        res += " ";
    }
    res += "]";
    return res;
};

// these two are helpers for Array.sort(func)
compare = function(l, r) {
    return (l == r ? 0 : (l < r ? -1 : 1));
};

// arr.sort(compareOn('name'))
compareOn = function(field) {
    return function(l, r) {
        return compare(l[field], r[field]);
    };
};

shellPrint = function(x) {
    it = x;
    if (x != undefined)
        shellPrintHelper(x);
};

_originalPrint = print;
disablePrint = function() {
    print = Function.prototype;
};

enablePrint = function() {
    print = _originalPrint;
};

print.captureAllOutput = function(fn, args) {
    var res = {};
    res.output = [];
    var __orig_print = print;
    print = function() {
        Array.prototype.push.apply(res.output,
                                   Array.prototype.slice.call(arguments).join(" ").split("\n"));
    };
    try {
        res.result = fn.apply(undefined, args);
    } finally {
        // Stop capturing print() output
        print = __orig_print;
    }
    return res;
};

var indentStr = function(indent, s) {
    if (typeof (s) === "undefined") {
        s = indent;
        indent = 0;
    }
    if (indent > 0) {
        indent = (new Array(indent + 1)).join(" ");
        s = indent + s.replace(/\n/g, "\n" + indent);
    }
    return s;
};

if (typeof TestData == "undefined") {
    TestData = undefined;
}

function __sanitizeMatch(flag) {
    var sanitizeMatch = /-fsanitize=([^\s]+) /.exec(getBuildInfo()["buildEnvironment"]["ccflags"]);
    if (flag && sanitizeMatch && RegExp(flag).exec(sanitizeMatch[1])) {
        return true;
    } else {
        return false;
    }
}

function _isAddressSanitizerActive() {
    return __sanitizeMatch("address");
}

function _isLeakSanitizerActive() {
    return __sanitizeMatch("leak");
}

function _isThreadSanitizerActive() {
    return __sanitizeMatch("thread");
}

function _isUndefinedBehaviorSanitizerActive() {
    return __sanitizeMatch("undefined");
}

jsTestName = function() {
    if (TestData)
        return TestData.testName;
    return "__unknown_name__";
};

var _jsTestOptions = {};

jsTestOptions = function() {
    if (TestData) {
        return Object.merge(_jsTestOptions, {
            // Test commands should be enabled by default if no enableTestCommands were present in
            // TestData
            enableTestCommands:
                TestData.hasOwnProperty('enableTestCommands') ? TestData.enableTestCommands : true,
            // Testing diagnostics should be enabled by default if no testingDiagnosticsEnabled was
            // present in TestData
            testingDiagnosticsEnabled: TestData.hasOwnProperty('testingDiagnosticsEnabled')
                ? TestData.testingDiagnosticsEnabled
                : true,
            setParameters: TestData.setParameters,
            setParametersMongos: TestData.setParametersMongos,
            setParametersMongocryptd: TestData.setParametersMongocryptd,
            storageEngine: TestData.storageEngine,
            storageEngineCacheSizeGB: TestData.storageEngineCacheSizeGB,
            transportLayer: TestData.transportLayer,
            wiredTigerEngineConfigString: TestData.wiredTigerEngineConfigString,
            wiredTigerCollectionConfigString: TestData.wiredTigerCollectionConfigString,
            wiredTigerIndexConfigString: TestData.wiredTigerIndexConfigString,
            auth: TestData.auth,
            // Note: keyFile is also used as a flag to indicate cluster auth is turned on, set it
            // to a truthy value if you'd like to do cluster auth, even if it's not keyFile auth.
            // Use clusterAuthMode to specify the actual auth mode you want to use.
            keyFile: TestData.keyFile,
            authUser: TestData.authUser || "__system",
            authPassword: TestData.keyFileData,
            authenticationDatabase: TestData.authenticationDatabase || "admin",
            authMechanism: TestData.authMechanism,
            clusterAuthMode: TestData.clusterAuthMode || "keyFile",
            adminUser: TestData.adminUser || "admin",
            adminPassword: TestData.adminPassword || "password",
            useLegacyConfigServers: TestData.useLegacyConfigServers || false,
            enableMajorityReadConcern: TestData.enableMajorityReadConcern,
            writeConcernMajorityShouldJournal: TestData.writeConcernMajorityShouldJournal,
            enableEncryption: TestData.enableEncryption,
            encryptionCipherMode: TestData.encryptionCipherMode,
            encryptionKeyFile: TestData.encryptionKeyFile,
            auditDestination: TestData.auditDestination,
            minPort: TestData.minPort,
            maxPort: TestData.maxPort,
            // Note: does not support the array version
            mongosBinVersion: TestData.mongosBinVersion || "",
            mongoqBinVersion: TestData.mongoqBinVersion || "",
            shardMixedBinVersions: TestData.shardMixedBinVersions || false,
            mixedBinVersions: TestData.mixedBinVersions || false,
            networkMessageCompressors: TestData.networkMessageCompressors,
            replSetFeatureCompatibilityVersion: TestData.replSetFeatureCompatibilityVersion,
            skipRetryOnNetworkError: TestData.skipRetryOnNetworkError,
            skipValidationOnInvalidViewDefinitions: TestData.skipValidationOnInvalidViewDefinitions,
            forceValidationWithFeatureCompatibilityVersion:
                TestData.forceValidationWithFeatureCompatibilityVersion,
            skipCollectionAndIndexValidation: TestData.skipCollectionAndIndexValidation,
            // We default skipValidationOnNamespaceNotFound to true because mongod can end up
            // dropping a collection after calling listCollections (e.g. if a secondary applies an
            // oplog entry).
            skipValidationOnNamespaceNotFound:
                TestData.hasOwnProperty("skipValidationOnNamespaceNotFound")
                ? TestData.skipValidationOnNamespaceNotFound
                : true,
            skipValidationNamespaces: TestData.skipValidationNamespaces || [],
            skipCheckingUUIDsConsistentAcrossCluster:
                TestData.skipCheckingUUIDsConsistentAcrossCluster || false,
            skipCheckingIndexesConsistentAcrossCluster:
                TestData.skipCheckingIndexesConsistentAcrossCluster || false,
            skipCheckingCatalogCacheConsistencyWithShardingCatalog:
                TestData.skipCheckingCatalogCacheConsistencyWithShardingCatalog || false,
            skipAwaitingReplicationOnShardsBeforeCheckingUUIDs:
                TestData.skipAwaitingReplicationOnShardsBeforeCheckingUUIDs || false,
            jsonSchemaTestFile: TestData.jsonSchemaTestFile,
            excludedDBsFromDBHash: TestData.excludedDBsFromDBHash,
            alwaysInjectTransactionNumber: TestData.alwaysInjectTransactionNumber,
            skipGossipingClusterTime: TestData.skipGossipingClusterTime || false,
            disableEnableSessions: TestData.disableEnableSessions,
            overrideRetryAttempts: TestData.overrideRetryAttempts || 0,
            logRetryAttempts: TestData.logRetryAttempts || false,
            connectionString: TestData.connectionString || "",
            skipCheckDBHashes: TestData.skipCheckDBHashes || false,
            traceExceptions: TestData.hasOwnProperty("traceExceptions") ? TestData.traceExceptions
                                                                        : true,
            transactionLifetimeLimitSeconds: TestData.transactionLifetimeLimitSeconds,
            mqlTestFile: TestData.mqlTestFile,
            mqlRootPath: TestData.mqlRootPath,
            disableImplicitSessions: TestData.disableImplicitSessions || false,
            setSkipShardingPartsOfPrepareTransactionFailpoint:
                TestData.setSkipShardingPartsOfPrepareTransactionFailpoint || false,
            roleGraphInvalidationIsFatal: TestData.roleGraphInvalidationIsFatal || false,
            networkErrorAndTxnOverrideConfig: TestData.networkErrorAndTxnOverrideConfig || {},
            // When useRandomBinVersionsWithinReplicaSet is true, randomly assign the binary
            // versions of each node in the replica set to 'latest' or 'last-lts'.
            // This flag is currently a placeholder and only sets the replica set to last-lts
            // FCV.
            useRandomBinVersionsWithinReplicaSet:
                TestData.useRandomBinVersionsWithinReplicaSet || false,
            // Set a specific random seed to be used when useRandomBinVersionsWithinReplicaSet is
            // true.
            seed: TestData.seed || undefined,
            // Override the logging options for mongod and mongos so they always log to a file
            // in dbpath; additionally, prevent the dbpath from being cleared after a node
            // is shut down.
            alwaysUseLogFiles: TestData.alwaysUseLogFiles || false,
            skipCheckOrphans: TestData.skipCheckOrphans || false,
            skipCheckRoutingTableConsistency: TestData.skipCheckRoutingTableConsistency || false,
            inEvergreen: TestData.inEvergreen || false,

            undoRecorderPath: TestData.undoRecorderPath,
            backupOnRestartDir: TestData.backupOnRestartDir || false,

            evergreenTaskId: TestData.evergreenTaskId || null,
        });
    }
    return _jsTestOptions;
};

jsTestLog = function(msg) {
    if (typeof msg === "object") {
        msg = tojson(msg);
    }
    assert.eq(typeof (msg), "string", "Received: " + msg);
    const msgs = ["----", ...msg.split("\n"), "----"].map(s => `[jsTest] ${s}`);
    print(`\n\n${msgs.join("\n")}\n\n`);
};

jsTest = {};

jsTest.name = jsTestName;
jsTest.options = jsTestOptions;
jsTest.log = jsTestLog;
jsTest.readOnlyUserRoles = ["read"];
jsTest.basicUserRoles = ["dbOwner"];
jsTest.adminUserRoles = ["root"];

jsTest.authenticate = function(conn) {
    const connOptions = conn.fullOptions || {};
    const authMode =
        connOptions.clusterAuthMode || conn.clusterAuthMode || jsTest.options().clusterAuthMode;

    if (!jsTest.options().auth && !jsTest.options().keyFile && authMode !== "x509") {
        conn.authenticated = true;
        return true;
    }

    try {
        assert.soon(function() {
            // Set authenticated to stop an infinite recursion from getDB calling
            // back into authenticate.
            conn.authenticated = true;
            let mech = DB.prototype._getDefaultAuthenticationMechanism();
            if (authMode === 'x509') {
                mech = 'MONGODB-X509';
            }

            print("Authenticating as user " + jsTestOptions().authUser + " with mechanism " + mech +
                  " on connection: " + conn);

            if (authMode !== 'x509') {
                conn.authenticated = conn.getDB(jsTestOptions().authenticationDatabase).auth({
                    user: jsTestOptions().authUser,
                    pwd: jsTestOptions().authPassword,
                });
            } else {
                authutil.assertAuthenticate(conn, '$external', {
                    mechanism: 'MONGODB-X509',
                });
            }

            return conn.authenticated;
            // Dont' run the hang analyzer because we expect that this might fail in the normal
            // course of events.
        }, "Authenticating connection: " + conn, 5000, 1000, {runHangAnalyzer: false});
    } catch (e) {
        print("Caught exception while authenticating connection: " + tojson(e));
        conn.authenticated = false;
    }
    return conn.authenticated;
};

jsTest.authenticateNodes = function(nodes) {
    assert.soonNoExcept(function() {
        for (var i = 0; i < nodes.length; i++) {
            // Don't try to authenticate to arbiters
            try {
                res = nodes[i].getDB("admin")._runCommandWithoutApiStrict({replSetGetStatus: 1});
            } catch (e) {
                // ReplicaSet tests which don't use auth are allowed to have nodes crash during
                // startup. To allow tests which use to behavior to work with auth,
                // attempting authentication against a dead node should be non-fatal.
                print("Caught exception getting replSetStatus while authenticating: " + e);
                continue;
            }
            if (res.myState == 7) {
                continue;
            }
            if (jsTest.authenticate(nodes[i]) != 1) {
                return false;
            }
        }
        return true;
    }, "Authenticate to nodes: " + nodes, 30000);
};

jsTest.isMongos = function(conn) {
    return conn.getDB('admin')._helloOrLegacyHello().msg == 'isdbgrid';
};

defaultPrompt = function() {
    var status = db.getMongo().authStatus;
    var prefix = db.getMongo().promptPrefix;

    if (typeof prefix == 'undefined') {
        prefix = "";
        var buildInfo = db._runCommandWithoutApiStrict({buildInfo: 1});
        try {
            if (buildInfo.modules.indexOf("enterprise") > -1) {
                prefix += "MongoDB Enterprise ";
            }
        } catch (e) {
            // Don't do anything here. Just throw the error away.
        }
        var hello = db._helloOrLegacyHello({forShell: 1});
        try {
            if (hello.hasOwnProperty("automationServiceDescriptor")) {
                prefix += "[automated] ";
            }
        } catch (e) {
            // Don't do anything here. Just throw the error away.
        }
        db.getMongo().promptPrefix = prefix;
    }

    try {
        // try to use repl set prompt -- no status or auth detected yet
        if (!status || !status.authRequired) {
            try {
                var prompt = replSetMemberStatePrompt();
                // set our status that it was good
                db.getMongo().authStatus = {replSetGetStatus: true, hello: true};
                return prefix + prompt;
            } catch (e) {
                // don't have permission to run that, or requires auth
                // print(e);
                status = {authRequired: true, replSetGetStatus: false, hello: true};
            }
        }
        // auth detected

        // try to use replSetGetStatus?
        if (status.replSetGetStatus) {
            try {
                var prompt = replSetMemberStatePrompt();
                // set our status that it was good
                status.replSetGetStatus = true;
                db.getMongo().authStatus = status;
                return prefix + prompt;
            } catch (e) {
                // don't have permission to run that, or requires auth
                // print(e);
                status.authRequired = true;
                status.replSetGetStatus = false;
            }
        }

        // try to use hello?
        if (status.hello) {
            try {
                var prompt = helloStatePrompt(hello);
                status.hello = true;
                db.getMongo().authStatus = status;
                return prefix + prompt;
            } catch (e) {
                status.authRequired = true;
                status.hello = false;
            }
        }
    } catch (ex) {
        printjson(ex);
        // reset status and let it figure it out next time.
        status = {hello: true};
    }

    db.getMongo().authStatus = status;
    return prefix + "> ";
};

replSetMemberStatePrompt = function() {
    var state = '';
    var stateInfo =
        db.getSiblingDB('admin')._runCommandWithoutApiStrict({replSetGetStatus: 1, forShell: 1});
    if (stateInfo.ok) {
        // Report the self member's stateStr if it's present.
        stateInfo.members.forEach(function(member) {
            if (member.self) {
                state = member.stateStr;
            }
        });
        // Otherwise fall back to reporting the numeric myState field (mongodb 1.6).
        if (!state) {
            state = stateInfo.myState;
        }
        state = '' + stateInfo.set + ':' + state;
    } else {
        var info = stateInfo.info;
        if (info && info.length < 20) {
            state = info;  // "mongos", "configsvr"
        } else {
            throw _getErrorWithCode(stateInfo, "Failed:" + info);
        }
    }
    return state + '> ';
};

helloStatePrompt = function(helloReply) {
    var state = '';
    var hello = helloReply || db._helloOrLegacyHello({forShell: 1});
    if (hello.ok) {
        var role = "";

        if (hello.msg == "isdbgrid") {
            role = "mongos";
        }

        if (hello.setName) {
            if (hello.isWritablePrimary || hello.ismaster)
                role = "PRIMARY";
            else if (hello.secondary)
                role = "SECONDARY";
            else if (hello.arbiterOnly)
                role = "ARBITER";
            else {
                role = "OTHER";
            }
            state = hello.setName + ':';
        }
        state = state + role;
    } else {
        throw _getErrorWithCode(hello, "Failed: " + tojson(hello));
    }
    return state + '> ';
};

if (typeof _shouldRetryWrites === 'undefined') {
    // We ensure the _shouldRetryWrites() function is always defined, in case the JavaScript engine
    // is being used from someplace other than the mongo shell (e.g. map-reduce).
    _shouldRetryWrites = function _shouldRetryWrites() {
        return false;
    };
}

if (typeof _shouldUseImplicitSessions === 'undefined') {
    // We ensure the _shouldUseImplicitSessions() function is always defined, in case the JavaScript
    // engine is being used from someplace other than the mongo shell (e.g. map-reduce). If the
    // function was not defined, implicit sessions are disabled to prevent unnecessary sessions from
    // being created.
    _shouldUseImplicitSessions = function _shouldUseImplicitSessions() {
        return false;
    };
}

shellPrintHelper = function(x) {
    if (typeof (x) == "undefined") {
        return;
    }

    if (x == __magicNoPrint)
        return;

    if (x == null) {
        print("null");
        return;
    }

    if (x === MinKey || x === MaxKey)
        return x.tojson();

    if (typeof x != "object")
        return print(x);

    var p = x.shellPrint;
    if (typeof p == "function")
        return x.shellPrint();

    var p = x.tojson;
    if (typeof p == "function")
        print(x.tojson());
    else
        print(tojson(x));
};

shellAutocomplete = function(
    /*prefix*/) {  // outer scope function called on init. Actual function at end
    var universalMethods =
        "constructor prototype toString valueOf toLocaleString hasOwnProperty propertyIsEnumerable"
            .split(' ');

    var builtinMethods = {};  // uses constructor objects as keys
    builtinMethods[Array] =
        "length concat join pop push reverse shift slice sort splice unshift indexOf lastIndexOf every filter forEach map some isArray reduce reduceRight"
            .split(' ');
    builtinMethods[Boolean] = "".split(' ');  // nothing more than universal methods
    builtinMethods[Date] =
        "getDate getDay getFullYear getHours getMilliseconds getMinutes getMonth getSeconds getTime getTimezoneOffset getUTCDate getUTCDay getUTCFullYear getUTCHours getUTCMilliseconds getUTCMinutes getUTCMonth getUTCSeconds getYear parse setDate setFullYear setHours setMilliseconds setMinutes setMonth setSeconds setTime setUTCDate setUTCFullYear setUTCHours setUTCMilliseconds setUTCMinutes setUTCMonth setUTCSeconds setYear toDateString toGMTString toISOString toLocaleDateString toLocaleTimeString toTimeString toUTCString UTC now"
            .split(' ');
    if (typeof JSON != "undefined") {  // JSON is new in V8
        builtinMethods["[object JSON]"] = "parse stringify".split(' ');
    }
    builtinMethods[Math] =
        "E LN2 LN10 LOG2E LOG10E PI SQRT1_2 SQRT2 abs acos asin atan atan2 ceil cos exp floor log max min pow random round sin sqrt tan"
            .split(' ');
    builtinMethods[Number] =
        "MAX_VALUE MIN_VALUE NEGATIVE_INFINITY POSITIVE_INFINITY toExponential toFixed toPrecision"
            .split(' ');
    builtinMethods[RegExp] =
        "global ignoreCase lastIndex multiline source compile exec test".split(' ');
    builtinMethods[String] =
        "length charAt charCodeAt concat fromCharCode indexOf lastIndexOf match replace search slice split substr substring toLowerCase toUpperCase trim trimLeft trimRight"
            .split(' ');
    builtinMethods[Function] = "call apply bind".split(' ');
    builtinMethods[Object] =
        "bsonsize create defineProperty defineProperties getPrototypeOf keys seal freeze preventExtensions isSealed isFrozen isExtensible getOwnPropertyDescriptor getOwnPropertyNames"
            .split(' ');

    builtinMethods[Mongo] = "find update insert remove".split(' ');
    builtinMethods[BinData] = "hex base64 length subtype".split(' ');

    var extraGlobals =
        "Infinity NaN undefined null true false decodeURI decodeURIComponent encodeURI encodeURIComponent escape eval isFinite isNaN parseFloat parseInt unescape Array Boolean Date Math Number RegExp String print load gc MinKey MaxKey Mongo NumberInt NumberLong ObjectId DBPointer UUID BinData HexData MD5 Map Timestamp JSON"
            .split(' ');
    if (typeof NumberDecimal !== 'undefined') {
        extraGlobals[extraGlobals.length] = "NumberDecimal";
    }

    var isPrivate = function(name) {
        if (shellAutocomplete.showPrivate)
            return false;
        if (name == '_id')
            return false;
        if (name[0] == '_')
            return true;
        if (name[name.length - 1] == '_')
            return true;  // some native functions have an extra name_ method
        return false;
    };

    var customComplete = function(obj) {
        try {
            if (obj.__proto__.constructor.autocomplete) {
                var ret = obj.constructor.autocomplete(obj);
                if (ret.constructor != Array) {
                    print("\nautocompleters must return real Arrays");
                    return [];
                }
                return ret;
            } else {
                return [];
            }
        } catch (e) {
            // print( e ); // uncomment if debugging custom completers
            return [];
        }
    };

    var worker = function(prefix) {
        var global = (function() {
                         return this;
                     }).call();  // trick to get global object

        var curObj = global;
        var parts = prefix.split('.');
        for (var p = 0; p < parts.length - 1; p++) {  // doesn't include last part
            curObj = curObj[parts[p]];
            if (curObj == null)
                return [];
        }

        var lastPrefix = parts[parts.length - 1] || '';
        var lastPrefixLowercase = lastPrefix.toLowerCase();
        var beginning = parts.slice(0, parts.length - 1).join('.');
        if (beginning.length)
            beginning += '.';

        var possibilities =
            new Array().concat(universalMethods,
                               Object.keySet(curObj),
                               Object.keySet(curObj.__proto__),
                               builtinMethods[curObj] || [],  // curObj is a builtin constructor
                               builtinMethods[curObj.__proto__.constructor] ||
                                   [],  // curObj is made from a builtin constructor
                               curObj == global ? extraGlobals : [],
                               customComplete(curObj));

        var noDuplicates =
            {};  // see http://dreaminginjavascript.wordpress.com/2008/08/22/eliminating-duplicates/
        for (var i = 0; i < possibilities.length; i++) {
            var p = possibilities[i];
            if (typeof (curObj[p]) == "undefined" && curObj != global)
                continue;  // extraGlobals aren't in the global object
            if (p.length == 0 || p.length < lastPrefix.length)
                continue;
            if (lastPrefix[0] != '_' && isPrivate(p))
                continue;
            if (p.match(/^[0-9]+$/))
                continue;  // don't array number indexes
            if (p.substr(0, lastPrefix.length).toLowerCase() != lastPrefixLowercase)
                continue;

            var completion = beginning + p;
            if (curObj[p] && curObj[p].constructor == Function && p != 'constructor')
                completion += '(';

            noDuplicates[completion] = 0;
        }

        var ret = [];
        for (var i in noDuplicates)
            ret.push(i);

        return ret;
    };

    // this is the actual function that gets assigned to shellAutocomplete
    return function(prefix) {
        try {
            __autocomplete__ = worker(prefix).sort();
        } catch (e) {
            print("exception during autocomplete: " + tojson(e.message));
            __autocomplete__ = [];
        }
    };
}();

shellAutocomplete.showPrivate = false;  // toggle to show (useful when working on internals)

shellHelper = function(command, rest, shouldPrint) {
    command = command.trim();
    var args = rest.trim().replace(/\s*;$/, "").split("\s+");

    if (!shellHelper[command])
        throw Error("no command [" + command + "]");

    var res = shellHelper[command].apply(null, args);
    if (shouldPrint) {
        shellPrintHelper(res);
    }
    return res;
};

shellHelper.use = function(dbname) {
    var s = "" + dbname;
    if (s == "") {
        print("bad use parameter");
        return;
    }
    db = db.getSiblingDB(dbname);
    print("switched to db " + db.getName());
};

shellHelper.set = function(str) {
    if (str == "") {
        print("bad use parameter");
        return;
    }
    tokens = str.split(" ");
    param = tokens[0];
    value = tokens[1];

    if (value == undefined)
        value = true;
    // value comes in as a string..
    if (value == "true")
        value = true;
    if (value == "false")
        value = false;

    if (param == "verbose") {
        _verboseShell = value;
    }
    print("set " + param + " to " + value);
};

shellHelper.it = function() {
    if (typeof (___it___) == "undefined" || ___it___ == null) {
        print("no cursor");
        return;
    }
    shellPrintHelper(___it___);
};

shellHelper.show = function(what) {
    assert(typeof what == "string");

    var args = what.split(/\s+/);
    what = args[0];
    args = args.splice(1);

    var messageIndent = "        ";

    if (what == "profile") {
        if (db.system.profile.count() == 0) {
            print("db.system.profile is empty");
            print("Use db.setProfilingLevel(2) will enable profiling");
            print("Use db.system.profile.find() to show raw profile entries");
        } else {
            print();
            db.system.profile.find({millis: {$gt: 0}})
                .sort({$natural: -1})
                .limit(5)
                .forEach(function(x) {
                    print("" + x.op + "\t" + x.ns + " " + x.millis + "ms " +
                          String(x.ts).substring(0, 24));
                    var l = "";
                    for (var z in x) {
                        if (z == "op" || z == "ns" || z == "millis" || z == "ts")
                            continue;

                        var val = x[z];
                        var mytype = typeof (val);

                        if (mytype == "string" || mytype == "number")
                            l += z + ":" + val + " ";
                        else if (mytype == "object")
                            l += z + ":" + tojson(val) + " ";
                        else if (mytype == "boolean")
                            l += z + " ";
                        else
                            l += z + ":" + val + " ";
                    }
                    print(l);
                    print("\n");
                });
        }
        return "";
    }

    if (what == "users") {
        db.getUsers().forEach(printjson);
        return "";
    }

    if (what == "roles") {
        db.getRoles({showBuiltinRoles: true}).forEach(printjson);
        return "";
    }

    if (what == "collections" || what == "tables") {
        db.getCollectionInfos({}, true, true).forEach(function(infoObj) {
            print(infoObj.name);
        });
        return "";
    }

    if (what == "dbs" || what == "databases") {
        var mongo = db.getMongo();
        var dbs;
        try {
            dbs = mongo.getDBs(db.getSession(), undefined, false);
        } catch (ex) {
            // Unable to get detailed information, retry name-only.
            mongo.getDBs(db.getSession(), undefined, true).forEach(function(x) {
                print(x);
            });
            return "";
        }

        var dbinfo = [];
        var maxNameLength = 0;
        var maxGbDigits = 0;

        dbs.databases.forEach(function(x) {
            var sizeStr = (x.sizeOnDisk / 1024 / 1024 / 1024).toFixed(3);
            var nameLength = x.name.length;
            var gbDigits = sizeStr.indexOf(".");

            if (nameLength > maxNameLength)
                maxNameLength = nameLength;
            if (gbDigits > maxGbDigits)
                maxGbDigits = gbDigits;

            dbinfo.push({
                name: x.name,
                size: x.sizeOnDisk,
                empty: x.empty,
                size_str: sizeStr,
                name_size: nameLength,
                gb_digits: gbDigits
            });
        });

        dbinfo.sort(compareOn('name'));
        dbinfo.forEach(function(db) {
            var namePadding = maxNameLength - db.name_size;
            var sizePadding = maxGbDigits - db.gb_digits;
            var padding = Array(namePadding + sizePadding + 3).join(" ");
            if (db.size > 1) {
                print(db.name + padding + db.size_str + "GB");
            } else if (db.empty) {
                print(db.name + padding + "(empty)");
            } else {
                print(db.name);
            }
        });

        return "";
    }

    if (what == "log") {
        var n = "global";
        if (args.length > 0)
            n = args[0];

        var res = db.adminCommand({getLog: n});
        if (!res.ok) {
            print("Error while trying to show " + n + " log: " + res.errmsg);
            return "";
        }
        for (var i = 0; i < res.log.length; i++) {
            print(res.log[i]);
        }
        return "";
    }

    if (what == "logs") {
        var res = db.adminCommand({getLog: "*"});
        if (!res.ok) {
            print("Error while trying to show logs: " + res.errmsg);
            return "";
        }
        for (var i = 0; i < res.names.length; i++) {
            print(res.names[i]);
        }
        return "";
    }

    if (what == "startupWarnings") {
        var dbDeclared, ex;
        try {
            // !!db essentially casts db to a boolean
            // Will throw a reference exception if db hasn't been declared.
            dbDeclared = !!db;
        } catch (ex) {
            dbDeclared = false;
        }
        if (dbDeclared) {
            var res =
                db.getSiblingDB("admin")._runCommandWithoutApiStrict({getLog: "startupWarnings"});
            if (res.ok) {
                if (res.log.length == 0) {
                    return "";
                }
                print("---");
                print("The server generated these startup warnings when booting: ");
                for (var i = 0; i < res.log.length; i++) {
                    var logOut;
                    try {
                        var parsedLog = JSON.parse(res.log[i]);
                        var linePrefix = messageIndent + parsedLog.t["$date"] + ": ";
                        logOut = linePrefix + parsedLog.msg + "\n";
                        if (parsedLog.attr) {
                            for (var attr in parsedLog.attr) {
                                logOut += linePrefix + messageIndent + attr + ": " +
                                    parsedLog.attr[attr] + "\n";
                            }
                        }
                    } catch (err) {
                        // err is intentionally unused here
                        logOut = res.log[i];
                    }
                    print(logOut);
                }
                print("---");
                return "";
            } else if (res.errmsg == "no such cmd: getLog") {
                // Don't print if the command is not available
                return "";
            } else if (res.code == 13 /*unauthorized*/ || res.errmsg == "unauthorized" ||
                       res.errmsg == "need to login") {
                // Don't print if startupWarnings command failed due to auth
                return "";
            } else {
                print("Error while trying to show server startup warnings: " + res.errmsg);
                return "";
            }
        } else {
            print("Cannot show startupWarnings, \"db\" is not set");
            return "";
        }
    }

    if (what == "automationNotices") {
        var dbDeclared, ex;
        try {
            // !!db essentially casts db to a boolean
            // Will throw a reference exception if db hasn't been declared.
            dbDeclared = !!db;
        } catch (ex) {
            dbDeclared = false;
        }

        if (dbDeclared) {
            var res = db._helloOrLegacyHello({forShell: 1});
            if (!res.ok) {
                print("Note: Cannot determine if automation is active");
                return "";
            }

            if (res.hasOwnProperty("automationServiceDescriptor")) {
                print("Note: This server is managed by automation service '" +
                      res.automationServiceDescriptor + "'.");
                print(
                    "Note: Many administrative actions are inappropriate, and may be automatically reverted.");
                return "";
            }

            return "";

        } else {
            print("Cannot show automationNotices, \"db\" is not set");
            return "";
        }
    }

    if (what == "freeMonitoring") {
        var dbDeclared, ex;
        try {
            // !!db essentially casts db to a boolean
            // Will throw a reference exception if db hasn't been declared.
            dbDeclared = !!db;
        } catch (ex) {
            dbDeclared = false;
        }

        if (dbDeclared) {
            const freemonStatus = db.adminCommand({getFreeMonitoringStatus: 1});

            if (freemonStatus.ok) {
                if (freemonStatus.state == 'enabled' &&
                    freemonStatus.hasOwnProperty('userReminder')) {
                    print("---");
                    print(freemonStatus.userReminder);
                    print("---");
                } else if (freemonStatus.state === 'undecided') {
                    print(
                        "---\n" + messageIndent +
                        "Enable MongoDB's free cloud-based monitoring service, which will then receive and display\n" +
                        messageIndent +
                        "metrics about your deployment (disk utilization, CPU, operation statistics, etc).\n" +
                        "\n" + messageIndent +
                        "The monitoring data will be available on a MongoDB website with a unique URL accessible to you\n" +
                        messageIndent +
                        "and anyone you share the URL with. MongoDB may use this information to make product\n" +
                        messageIndent +
                        "improvements and to suggest MongoDB products and deployment options to you.\n" +
                        "\n" + messageIndent +
                        "To enable free monitoring, run the following command: db.enableFreeMonitoring()\n" +
                        messageIndent +
                        "To permanently disable this reminder, run the following command: db.disableFreeMonitoring()\n" +
                        "---\n");
                }
            }

            return "";
        } else {
            print("Cannot show freeMonitoring, \"db\" is not set");
            return "";
        }
    }

    if (what == "nonGenuineMongoDBCheck") {
        let matchesKnownImposterSignature = false;

        // A MongoDB emulation service offered by a company
        // responsible for a certain disk operating system.
        try {
            const buildInfo = db._runCommandWithoutApiStrict({buildInfo: 1});
            if (buildInfo.hasOwnProperty('_t')) {
                matchesKnownImposterSignature = true;
            }
        } catch (e) {
            // Don't do anything here. Just throw the error away.
        }

        // A MongoDB emulation service offered by a company named
        // after some sort of minor river or something.
        if (!matchesKnownImposterSignature) {
            try {
                const cmdLineOpts = db.adminCommand({getCmdLineOpts: 1});
                if (cmdLineOpts.hasOwnProperty('errmsg') &&
                    cmdLineOpts.errmsg.indexOf('not supported') !== -1) {
                    matchesKnownImposterSignature = true;
                }
            } catch (e) {
                // Don't do anything here. Just throw the error away.
            }
        }

        if (matchesKnownImposterSignature) {
            print("\n" +
                  "Warning: Non-Genuine MongoDB Detected\n\n" +

                  "This server or service appears to be an emulation of MongoDB " +
                  "rather than an official MongoDB product.\n\n" +

                  "Some documented MongoDB features may work differently, " +
                  "be entirely missing or incomplete, " +
                  "or have unexpected performance characteristics.\n\n" +

                  "To learn more please visit: " +
                  "https://dochub.mongodb.org/core/non-genuine-mongodb-server-warning.\n");
        }

        return "";
    }

    throw Error("don't know how to show [" + what + "]");
};

__promptWrapper__ = function(promptFunction) {
    // Call promptFunction directly if the global "db" is not defined, e.g. --nodb.
    if (typeof db === 'undefined' || !(db instanceof DB)) {
        __prompt__ = promptFunction();
        return;
    }

    // Stash the global "db" for the prompt function to make sure the session
    // of the global "db" isn't accessed by the prompt function.
    let originalDB = db;
    try {
        db = originalDB.getMongo().getDB(originalDB.getName());
        // Setting db._session to be a _DummyDriverSession instance makes it so that
        // a logical session id isn't included in the hello and replSetGetStatus
        // commands and therefore won't interfere with the session associated with the
        // global "db" object.
        db._session = new _DummyDriverSession(db.getMongo());
        __prompt__ = promptFunction();
    } finally {
        db = originalDB;
    }
};

Math.sigFig = function(x, N) {
    if (!N) {
        N = 3;
    }
    var p = Math.pow(10, N - Math.ceil(Math.log(Math.abs(x)) / Math.log(10)));
    return Math.round(x * p) / p;
};

var Random = (function() {
    var initialized = false;
    var errorMsg = "The random number generator hasn't been seeded yet; " +
        "call Random.setRandomSeed()";

    function isInitialized() {
        return initialized;
    }

    // Set the random generator seed.
    function srand(s) {
        initialized = true;
        return _srand(s);
    }

    // Set the random generator seed & print the result.
    function setRandomSeed(s) {
        var seed = srand(s);
        print("setting random seed: " + seed);
    }

    // Generate a random number 0 <= r < 1.
    function rand() {
        if (!initialized) {
            throw new Error(errorMsg);
        }
        return _rand();
    }

    // Generate a random integer 0 <= r < n.
    function randInt(n) {
        if (!initialized) {
            throw new Error(errorMsg);
        }
        return Math.floor(rand() * n);
    }

    // Generate a random value from the exponential distribution with the specified mean.
    function genExp(mean) {
        if (!initialized) {
            throw new Error(errorMsg);
        }
        var r = rand();
        if (r == 0) {
            r = rand();
            if (r == 0) {
                r = 0.000001;
            }
        }
        return -Math.log(r) * mean;
    }

    /**
     * Generate a random value from the normal distribution with specified 'mean' and
     * 'standardDeviation'.
     */
    function genNormal(mean, standardDeviation) {
        if (!initialized) {
            throw new Error(errorMsg);
        }
        // See http://en.wikipedia.org/wiki/Marsaglia_polar_method
        while (true) {
            var x = (2 * rand()) - 1;
            var y = (2 * rand()) - 1;
            var s = (x * x) + (y * y);

            if (s > 0 && s < 1) {
                var standardNormal = x * Math.sqrt(-2 * Math.log(s) / s);
                return mean + (standardDeviation * standardNormal);
            }
        }
    }

    return {
        genExp: genExp,
        genNormal: genNormal,
        isInitialized: isInitialized,
        rand: rand,
        randInt: randInt,
        setRandomSeed: setRandomSeed,
        srand: srand,
    };
})();

/**
 * Compares Timestamp objects. Returns -1 if ts1 is 'earlier' than ts2, 1 if 'later'
 * and 0 if equal.
 */
function timestampCmp(ts1, ts2) {
    if (ts1.getTime() == ts2.getTime()) {
        if (ts1.getInc() < ts2.getInc()) {
            return -1;
        } else if (ts1.getInc() > ts2.getInc()) {
            return 1;
        } else {
            return 0;
        }
    } else if (ts1.getTime() < ts2.getTime()) {
        return -1;
    } else {
        return 1;
    }
}

Geo = {};
Geo.distance = function(a, b) {
    var ax = null;
    var ay = null;
    var bx = null;
    var by = null;

    for (var key in a) {
        if (ax == null)
            ax = a[key];
        else if (ay == null)
            ay = a[key];
    }

    for (var key in b) {
        if (bx == null)
            bx = b[key];
        else if (by == null)
            by = b[key];
    }

    return Math.sqrt(Math.pow(by - ay, 2) + Math.pow(bx - ax, 2));
};

Geo.sphereDistance = function(a, b) {
    var ax = null;
    var ay = null;
    var bx = null;
    var by = null;

    // TODO swap order of x and y when done on server
    for (var key in a) {
        if (ax == null)
            ax = a[key] * (Math.PI / 180);
        else if (ay == null)
            ay = a[key] * (Math.PI / 180);
    }

    for (var key in b) {
        if (bx == null)
            bx = b[key] * (Math.PI / 180);
        else if (by == null)
            by = b[key] * (Math.PI / 180);
    }

    var sin_x1 = Math.sin(ax), cos_x1 = Math.cos(ax);
    var sin_y1 = Math.sin(ay), cos_y1 = Math.cos(ay);
    var sin_x2 = Math.sin(bx), cos_x2 = Math.cos(bx);
    var sin_y2 = Math.sin(by), cos_y2 = Math.cos(by);

    var cross_prod = (cos_y1 * cos_x1 * cos_y2 * cos_x2) + (cos_y1 * sin_x1 * cos_y2 * sin_x2) +
        (sin_y1 * sin_y2);

    if (cross_prod >= 1 || cross_prod <= -1) {
        // fun with floats
        assert(Math.abs(cross_prod) - 1 < 1e-6);
        return cross_prod > 0 ? 0 : Math.PI;
    }

    return Math.acos(cross_prod);
};

rs = function() {
    return "try rs.help()";
};

/**
 * This method is intended to aid in the writing of tests. It takes a host's address, desired state,
 * and replicaset and waits either timeout milliseconds or until that reaches the desired state.
 *
 * It should be used instead of awaitRSClientHost when there is no MongoS with a connection to the
 * replica set.
 */
_awaitRSHostViaRSMonitor = function(hostAddr, desiredState, rsName, timeout) {
    timeout = timeout || 60 * 1000;

    if (desiredState == undefined) {
        desiredState = {ok: true};
    }

    print("Awaiting " + hostAddr + " to be " + tojson(desiredState) + " in " +
          " rs " + rsName);

    var tests = 0;
    assert.soon(
        function() {
            var stats = _replMonitorStats(rsName);
            if (tests++ % 10 == 0) {
                printjson(stats);
            }

            for (var i = 0; i < stats.length; i++) {
                var node = stats[i];
                printjson(node);
                if (node["addr"] !== hostAddr)
                    continue;

                // Check that *all* hostAddr properties match desiredState properties
                var stateReached = true;
                for (var prop in desiredState) {
                    if (isObject(desiredState[prop])) {
                        if (!friendlyEqual(sortDoc(desiredState[prop]), sortDoc(node[prop]))) {
                            stateReached = false;
                            break;
                        }
                    } else if (node[prop] !== desiredState[prop]) {
                        stateReached = false;
                        break;
                    }
                }
                if (stateReached) {
                    printjson(stats);
                    return true;
                }
            }
            return false;
        },
        "timed out waiting for replica set member: " + hostAddr +
            " to reach state: " + tojson(desiredState),
        timeout);
};

rs.help = function() {
    print(
        "\trs.status()                                     { replSetGetStatus : 1 } checks repl set status");
    print(
        "\trs.initiate()                                   { replSetInitiate : null } initiates set with default settings");
    print(
        "\trs.initiate(cfg)                                { replSetInitiate : cfg } initiates set with configuration cfg");
    print(
        "\trs.conf()                                       get the current configuration object from local.system.replset");
    print(
        "\trs.reconfig(cfg, opts)                          updates the configuration of a running replica set with cfg, using the given opts (disconnects)");
    print(
        "\trs.reconfigForPSASet(memberIndex, cfg, opts)    updates the configuration of a Primary-Secondary-Arbiter (PSA) replica set while preserving majority writes");
    print(
        "\t                                                    memberIndex: index of the node being updated; cfg: the desired new config; opts: options passed in with the reconfig");
    print(
        "\t                                                    Not to be used with every configuration");
    print(
        "\t                                                    For more information, visit: https://docs.mongodb.com/manual/reference/method/rs.reconfigForPSASet/");
    print(
        "\trs.add(hostportstr)                             add a new member to the set with default attributes (disconnects)");
    print(
        "\trs.add(membercfgobj)                            add a new member to the set with extra attributes (disconnects)");
    print(
        "\trs.addArb(hostportstr)                          add a new member which is arbiterOnly:true (disconnects)");
    print("\trs.stepDown([stepdownSecs, catchUpSecs])        step down as primary (disconnects)");
    print(
        "\trs.syncFrom(hostportstr)                        make a secondary sync from the given member");
    print(
        "\trs.freeze(secs)                                 make a node ineligible to become primary for the time specified");
    print(
        "\trs.remove(hostportstr)                          remove a host from the replica set (disconnects)");
    print("\trs.secondaryOk()                                allow queries on secondary nodes");
    print();
    print("\trs.printReplicationInfo()                       check oplog size and time range");
    print(
        "\trs.printSecondaryReplicationInfo()              check replica set members and replication lag");
    print("\tdb.isMaster()                                   check who is primary");
    print("\tdb.hello()                                      check who is primary");
    print();
    print("\treconfiguration helpers disconnect from the database so the shell will display");
    print("\tan error, even if the command succeeds.");
};
rs.slaveOk = function(value) {
    print(
        "WARNING: slaveOk() is deprecated and may be removed in the next major release. Please use secondaryOk() instead.");
    return db.getMongo().setSecondaryOk(value);
};

rs.secondaryOk = function(value) {
    return db.getMongo().setSecondaryOk(value);
};

rs.status = function() {
    return db._adminCommand("replSetGetStatus");
};
rs.isMaster = function() {
    return db.isMaster();
};
rs.hello = function() {
    return db.hello();
};
rs.initiate = function(c) {
    return db._adminCommand({replSetInitiate: c});
};
rs.printSlaveReplicationInfo = function() {
    print(
        "WARNING: printSlaveReplicationInfo is deprecated and may be removed in the next major release. Please use printSecondaryReplicationInfo instead.");
    return db.printSecondaryReplicationInfo();
};
rs.printSecondaryReplicationInfo = function() {
    return db.printSecondaryReplicationInfo();
};
rs.printReplicationInfo = function() {
    return db.printReplicationInfo();
};
rs._runCmd = function(c) {
    // after the command, catch the disconnect and reconnect if necessary
    var res = null;
    try {
        res = db.adminCommand(c);
    } catch (e) {
        if (isNetworkError(e)) {
            if (reconnect(db)) {
                print("reconnected to server after rs command (which is normal)");
            } else {
                print("failed to reconnect to server after:" + e);
            }
        } else {
            print("shell got exception during repl set operation: " + e);
            print(
                "in some circumstances, the primary steps down and closes connections on a reconfig");
        }
        return "";
    }
    return res;
};
rs.reconfig = function(cfg, options) {
    cfg.version = rs.conf().version + 1;
    cmd = {replSetReconfig: cfg};
    for (var i in options) {
        cmd[i] = options[i];
    }
    return this._runCmd(cmd);
};

_validateMemberIndex = function(memberIndex, newConfig) {
    const newMemberConfig = newConfig.members[memberIndex];
    assert(newMemberConfig, `Node at index ${memberIndex} does not exist in the new config`);
    assert.eq(1,
              newMemberConfig.votes,
              `Node at index ${memberIndex} must have {votes: 1} in the new config`);

    // Use memberId to compare nodes across configs.
    const memberId = newMemberConfig._id;
    const oldConfig = rs.conf();
    const oldMemberConfig = oldConfig.members.find(member => member._id === memberId);

    // If the node doesn't exist in the old config, we are adding it as a new node. Skip validating
    // the node in the old config.
    if (!oldMemberConfig) {
        return;
    }

    assert(!oldMemberConfig.votes,
           `Node at index ${memberIndex} must have {votes: 0} in the old config`);
};

rs.reconfigForPSASet = function(memberIndex, cfg, options) {
    _validateMemberIndex(memberIndex, cfg);

    const memberPriority = cfg.members[memberIndex].priority;
    print(
        `Running first reconfig to give member at index ${memberIndex} { votes: 1, priority: 0 }`);
    cfg.members[memberIndex].votes = 1;
    cfg.members[memberIndex].priority = 0;
    let res = rs.reconfig(cfg, options);
    if (!res.ok) {
        return res;
    }

    print(`Running second reconfig to give member at index ${memberIndex} { priority: ${
        memberPriority} }`);
    cfg.members[memberIndex].priority = memberPriority;

    // If the first reconfig added a new node, the second config will not succeed until the
    // automatic reconfig to remove the 'newlyAdded' field is completed. Retry the second reconfig
    // until it succeeds in that case.
    assert.soon(() => {
        res = rs.reconfig(cfg, options);
        return res.ok;
    });
    return res;
};
rs.add = function(hostport, arb) {
    let res;
    let self = this;

    assert.soon(function() {
        var cfg = hostport;

        var local = db.getSiblingDB("local");
        assert(local.system.replset.count() <= 1,
               "error: local.system.replset has unexpected contents");
        var c = local.system.replset.findOne();
        assert(c, "no config object retrievable from local.system.replset");

        const attemptedVersion = c.version++;

        var max = 0;
        for (var i in c.members) {
            // Omit 'newlyAdded' field if it exists in the config.
            delete c.members[i].newlyAdded;
            if (c.members[i]._id > max)
                max = c.members[i]._id;
        }
        if (isString(hostport)) {
            cfg = {_id: max + 1, host: hostport};
            if (arb)
                cfg.arbiterOnly = true;
        } else if (arb == true) {
            throw Error(
                "Expected first parameter to be a host-and-port string of arbiter, but got " +
                tojson(hostport));
        }

        if (cfg._id == null) {
            cfg._id = max + 1;
        }
        c.members.push(cfg);

        res = self._runCmd({replSetReconfig: c});
        if (res === "") {
            // _runCmd caught an exception.
            return true;
        }
        if (res.ok) {
            return true;
        }
        if (res.code === ErrorCodes.ConfigurationInProgress ||
            res.code === ErrorCodes.CurrentConfigNotCommittedYet) {
            return false;  // keep retrying
        }
        if (res.code === ErrorCodes.NewReplicaSetConfigurationIncompatible) {
            // We will retry only if this error was due to our config version being too low.
            const cfgState = local.system.replset.findOne();
            if (cfgState.version >= attemptedVersion) {
                return false;  // keep retrying
            }
        }
        // Take no action on other errors.
        return true;
    }, () => tojson(res), 10 * 60 * 1000 /* timeout */, 200 /* interval */);

    return res;
};
rs.syncFrom = function(host) {
    return db._adminCommand({replSetSyncFrom: host});
};
rs.stepDown = function(stepdownSecs, catchUpSecs) {
    var cmdObj = {replSetStepDown: stepdownSecs === undefined ? 60 : stepdownSecs};
    if (catchUpSecs !== undefined) {
        cmdObj['secondaryCatchUpPeriodSecs'] = catchUpSecs;
    }
    return db._adminCommand(cmdObj);
};
rs.freeze = function(secs) {
    return db._adminCommand({replSetFreeze: secs});
};
rs.addArb = function(hn) {
    return this.add(hn, true);
};

rs.conf = function() {
    var resp = db._adminCommand({replSetGetConfig: 1});
    if (resp.ok && !(resp.errmsg) && resp.config)
        return resp.config;
    else if (resp.errmsg && resp.errmsg.startsWith("no such cmd"))
        return db.getSiblingDB("local").system.replset.findOne();
    throw new Error("Could not retrieve replica set config: " + tojson(resp));
};
rs.config = rs.conf;

rs.remove = function(hn) {
    var local = db.getSiblingDB("local");
    assert(local.system.replset.count() <= 1,
           "error: local.system.replset has unexpected contents");
    var c = local.system.replset.findOne();
    assert(c, "no config object retrievable from local.system.replset");
    c.version++;

    for (var i in c.members) {
        if (c.members[i].host == hn) {
            c.members.splice(i, 1);
            return db._adminCommand({replSetReconfig: c});
        }
    }

    return "error: couldn't find " + hn + " in " + tojson(c.members);
};

rs.debug = {};

rs.debug.nullLastOpWritten = function(primary, secondary) {
    var p = connect(primary + "/local");
    var s = connect(secondary + "/local");
    s.getMongo().setSecondaryOk();

    var secondToLast = s.oplog.rs.find().sort({$natural: -1}).limit(1).next();
    var last = p.runCommand({
        findAndModify: "oplog.rs",
        query: {ts: {$gt: secondToLast.ts}},
        sort: {$natural: 1},
        update: {$set: {op: "n"}}
    });

    if (!last.value.o || !last.value.o._id) {
        print("couldn't find an _id?");
    } else {
        last.value.o = {_id: last.value.o._id};
    }

    print("nulling out this op:");
    printjson(last);
};

rs.debug.getLastOpWritten = function(server) {
    var s = db.getSiblingDB("local");
    if (server) {
        s = connect(server + "/local");
    }
    s.getMongo().setSecondaryOk();

    return s.oplog.rs.find().sort({$natural: -1}).limit(1).next();
};

rs.isValidOpTime = function(opTime) {
    let timestampIsValid = (opTime.hasOwnProperty("ts") && (opTime.ts !== Timestamp(0, 0)));
    let termIsValid = (opTime.hasOwnProperty("t") && (opTime.t != -1));

    return timestampIsValid && termIsValid;
};

/**
 * Compares OpTimes in the format {ts:Timestamp, t:NumberLong}.
 * Returns -1 if ot1 is 'earlier' than ot2, 1 if 'later' and 0 if equal.
 */
rs.compareOpTimes = function(ot1, ot2) {
    if (!rs.isValidOpTime(ot1) || !rs.isValidOpTime(ot2)) {
        throw Error("invalid optimes, received: " + tojson(ot1) + " and " + tojson(ot2));
    }

    if (ot1.t > ot2.t) {
        return 1;
    } else if (ot1.t < ot2.t) {
        return -1;
    } else {
        return timestampCmp(ot1.ts, ot2.ts);
    }
};

help = shellHelper.help = function(x) {
    if (x == "mr") {
        print("\nSee also http://dochub.mongodb.org/core/mapreduce");
        print("\nfunction mapf() {");
        print("  // 'this' holds current document to inspect");
        print("  emit(key, value);");
        print("}");
        print("\nfunction reducef(key,value_array) {");
        print("  return reduced_value;");
        print("}");
        print("\ndb.mycollection.mapReduce(mapf, reducef[, options])");
        print("\noptions");
        print("{[query : <query filter object>]");
        print(" [, sort : <sort the query.  useful for optimization>]");
        print(" [, limit : <number of objects to return from collection>]");
        print(" [, out : <output-collection name>]");
        print(" [, keeptemp: <true|false>]");
        print(" [, finalize : <finalizefunction>]");
        print(" [, scope : <object where fields go into javascript global scope >]");
        print(" [, verbose : true]}\n");
        return;
    } else if (x == "connect") {
        print(
            "\nNormally one specifies the server on the mongo shell command line.  Run mongo --help to see those options.");
        print("Additional connections may be opened:\n");
        print("    var x = new Mongo('host[:port]');");
        print("    var mydb = x.getDB('mydb');");
        print("  or");
        print("    var mydb = connect('host[:port]/mydb');");
        return;
    } else if (x == "keys") {
        print("Tab completion and command history is available at the command prompt.\n");
        print("Some emacs keystrokes are available too:");
        print("  Ctrl-A start of line");
        print("  Ctrl-E end of line");
        print("  Ctrl-K del to end of line");
        print("\nMulti-line commands");
        print(
            "You can enter a multi line javascript expression.  If parens, braces, etc. are not closed, you will see a new line ");
        print(
            "beginning with '...' characters.  Type the rest of your expression.  Press Ctrl-C to abort the data entry if you");
        print("get stuck.\n");
    } else if (x == "misc") {
        print("\tb = new BinData(subtype,base64str)  create a BSON BinData value");
        print("\tb.subtype()                         the BinData subtype (0..255)");
        print("\tb.length()                          length of the BinData data in bytes");
        print("\tb.hex()                             the data as a hex encoded string");
        print("\tb.base64()                          the data as a base 64 encoded string");
        print("\tb.toString()");
        print();
        print(
            "\tb = HexData(subtype,hexstr)         create a BSON BinData value from a hex string");
        print("\tb = UUID(hexstr)                    create a BSON BinData value of UUID subtype");
        print("\tb = MD5(hexstr)                     create a BSON BinData value of MD5 subtype");
        print(
            "\t\"hexstr\"                            string, sequence of hex characters (no 0x prefix)");
        print();
        print("\to = new ObjectId()                  create a new ObjectId");
        print(
            "\to.getTimestamp()                    return timestamp derived from first 32 bits of the OID");
        print("\to.isObjectId");
        print("\to.toString()");
        print("\to.equals(otherid)");
        print();
        print(
            "\td = ISODate()                       like Date() but behaves more intuitively when used");
        print(
            "\td = ISODate('YYYY-MM-DD hh:mm:ss')    without an explicit \"new \" prefix on construction");
        return;
    } else if (x == "admin") {
        print("\tls([path])                      list files");
        print("\tpwd()                           returns current directory");
        print("\tlistFiles([path])               returns file list");
        print("\thostname()                      returns name of this host");
        print("\tcat(fname)                      returns contents of text file as a string");
        print("\tremoveFile(f)                   delete a file or directory");
        print("\tload(jsfilename)                load and execute a .js file");
        print("\trun(program[, args...])         spawn a program and wait for its completion");
        print("\trunProgram(program[, args...])  same as run(), above");
        print("\tsleep(m)                        sleep m milliseconds");
        print("\tgetMemInfo()                    diagnostic");
        return;
    } else if (x == "test") {
        print("\tMongoRunner.runMongod(args)   DELETES DATA DIR and then starts mongod");
        print("\t                              returns a connection to the new server");
        return;
    } else if (x == "") {
        print("\t" +
              "db.help()                    help on db methods");
        print("\t" +
              "db.mycoll.help()             help on collection methods");
        print("\t" +
              "sh.help()                    sharding helpers");
        print("\t" +
              "rs.help()                    replica set helpers");
        print("\t" +
              "help admin                   administrative help");
        print("\t" +
              "help connect                 connecting to a db help");
        print("\t" +
              "help keys                    key shortcuts");
        print("\t" +
              "help misc                    misc things to know");
        print("\t" +
              "help mr                      mapreduce");
        print();
        print("\t" +
              "show dbs                     show database names");
        print("\t" +
              "show collections             show collections in current database");
        print("\t" +
              "show users                   show users in current database");
        print(
            "\t" +
            "show profile                 show most recent system.profile entries with time >= 1ms");
        print("\t" +
              "show logs                    show the accessible logger names");
        print(
            "\t" +
            "show log [name]              prints out the last segment of log in memory, 'global' is default");
        print("\t" +
              "use <db_name>                set current database");
        print("\t" +
              "db.mycoll.find()             list objects in collection mycoll");
        print("\t" +
              "db.mycoll.find( { a : 1 } )  list objects in mycoll where a == 1");
        print(
            "\t" +
            "it                           result of the last line evaluated; use to further iterate");
        print("\t" +
              "DBQuery.shellBatchSize = x   set default number of items to display on shell");
        print("\t" +
              "exit                         quit the mongo shell");
    } else
        print("unknown help option");
};
