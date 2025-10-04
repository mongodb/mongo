const SIGTERM = 15;

let shellVersion = version;

// Record the exit codes of mongod and mongos processes that crashed during startup keyed by
// port. This map is cleared when MongoRunner._startWithArgs and MongoRunner.stopMongod/s are
// called.
let serverExitCodeMap = {};
let grpcToMongoRpcPortMap = {};

// A path.join-like thing for paths that must work
// on Windows (\-separated) and *nix (/-separated).
function pathJoin(...parts) {
    const separator = _isWindows() ? "\\" : "/";
    return parts.join(separator);
}

function MongoRunner() {}

MongoRunner.dataDir = "/data/db";
MongoRunner.dataPath = "/data/db/";

/**
 * Get the absolute path of a relative path located in the install directory.
 * Note that if the install directory is not defined, this returns the relative path.
 *
 * @param  {...any} parts string components of the relative path
 * @returns joined path prefixed by the install directory
 */
MongoRunner.getInstallPath = function (...parts) {
    const relativePath = pathJoin(...parts);
    const installDir = _getEnv("INSTALL_DIR");
    return installDir ? pathJoin(installDir, relativePath) : relativePath;
};

function getMongoSuffixPath(binary_name) {
    if (!jsTestOptions().inEvergreen) {
        return MongoRunner.getInstallPath(binary_name);
    }
    return binary_name;
}

MongoRunner.getMongodPath = function () {
    return getMongoSuffixPath("mongod");
};

MongoRunner.getMongosPath = function () {
    return getMongoSuffixPath("mongos");
};

MongoRunner.getMongoShellPath = function () {
    let shellPath = getMongoSuffixPath("mongo");
    return shellPath;
};

MongoRunner.getExtensionPath = function (shared_library_name) {
    if (!jsTestOptions().inEvergreen) {
        return MongoRunner.getInstallPath("..", "lib", shared_library_name);
    }
    return shared_library_name;
};

MongoRunner.parsePort = function (...args) {
    let port = "";
    const portKey = jsTestOptions().shellGRPC ? "--grpcPort" : "--port";

    for (let i = 0; i < args.length; ++i) {
        if (args[i] == portKey) {
            port = args[i + 1];
        }
    }

    if (port == "") throw Error("No port specified");
    return port;
};

MongoRunner.VersionSub = function (pattern, version) {
    this.pattern = pattern;
    this.version = version;
};

(function () {
    // Hang Analyzer integration.

    function getPids() {
        let pids = [];
        if (typeof TestData !== "undefined" && typeof TestData.peerPids !== "undefined") {
            pids = pids.concat(TestData.peerPids);
        }
        pids = pids.concat(MongoRunner.runningChildPids());
        return pids;
    }

    // Internal state to determine if the hang analyzer should be enabled or not.
    // Accessible via global setter/getter defined below.
    let _hangAnalyzerEnabled = true;

    /**
     * Run `./buildscripts/resmoke.py hang-analyzer`.
     *
     * @param {Number[]} pids
     *     optional pids of processes to pass to the hang analyzer.
     *     If not specified will use `TestData.peerPids` (pids of
     *     "fixture" processes started and passed in by resmoke)
     *     plus `MongoRunner.runningChildPids()` which includes all
     *     child processes started by `MongoRunner.runMongo*()` etc.
     */
    function runHangAnalyzer(pids) {
        if (typeof TestData === "undefined") {
            print("Skipping runHangAnalyzer: no TestData (not running from resmoke)");
            return;
        }

        if (!TestData.inEvergreen) {
            print("Skipping runHangAnalyzer: not running in Evergreen");
            return;
        }

        if (!_hangAnalyzerEnabled) {
            print("Skipping runHangAnalyzer: manually disabled");
            return;
        }

        if (typeof pids === "undefined") {
            pids = getPids();
        }
        if (pids.length <= 0) {
            print("Skipping runHangAnalyzer: no running child or peer mongo processes.");
            return;
        }
        // Result of runningChildPids may be NumberLong(), so
        // add 0 to convert to Number.
        pids = pids.map((p) => p + 0).join(",");
        print(`Running hang analyzer for pids [${pids}]`);

        const scriptPath = pathJoin(".", "buildscripts", "resmoke.py");
        // We are using a raw "python" rather than selecting the approperate python here
        // This is because as part of SERVER-79663 we noticed that servers.js is included in the legacy
        // shell
        // See hang-analyzer argument options here:
        // https://github.com/10gen/mongo/blob/8636ede10bd70b32ff4b6cd115132ab0f22b89c7/buildscripts/resmokelib/hang_analyzer/hang_analyzer.py#L245
        const args = ["python", scriptPath, "hang-analyzer", "-c", "-k", "-o", "file", "-o", "stdout", "-d", pids];

        if (jsTest.options().evergreenTaskId) {
            args.push("-t", jsTest.options().evergreenTaskId);
        }

        return runProgram(...args);
    }

    MongoRunner.runHangAnalyzer = runHangAnalyzer;

    MongoRunner.runHangAnalyzer.enable = function () {
        _hangAnalyzerEnabled = true;
    };

    MongoRunner.runHangAnalyzer.disable = function () {
        _hangAnalyzerEnabled = false;
    };
})();

/**
 * Returns an array of version elements from a version string.
 *
 * "3.3.4-fade3783" -> ["3", "3", "4-fade3783" ]
 * "3.2" -> [ "3", "2" ]
 * 3 -> exception: versions must have at least two components.
 */
let convertVersionStringToArray = function (versionString) {
    assert("" !== versionString, "Version strings must not be empty");
    let versionArray = versionString.split(".");

    assert.gt(
        versionArray.length,
        1,
        'MongoDB versions must have at least two components to compare, but "' +
            versionString +
            '" has ' +
            versionArray.length,
    );
    return versionArray;
};

/**
 * Returns an integer
 * This function will not work if the minor version is greater than or equal to 10
 */
let convertVersionStringToInteger = function (versionString) {
    const [major, minor] = _convertVersionToIntegerArray(versionString);
    assert(
        minor < 10,
        `Cannot convert a minor version greater than ten to an integer value since the minor version is in the tens position in the integer. Minor version attempting to convert: ${
            minor
        }`,
    );
    return major * 100 + minor * 10;
};

// These patterns allow substituting the binary versions used for each version string to support
// the dev/stable MongoDB release cycle.
let fcvConstants = getFCVConstants();

MongoRunner.binVersionSubs = [
    new MongoRunner.VersionSub("latest", shellVersion()),
    new MongoRunner.VersionSub("last-continuous", fcvConstants.lastContinuous),
    new MongoRunner.VersionSub("last-lts", fcvConstants.lastLTS),
];

MongoRunner.getBinVersionFor = function (version) {
    if (version instanceof MongoRunner.versionIterator.iterator) {
        version = version.current();
    }

    // Undefined version defaults to latest.
    if (version == null) version = "";
    version = version.trim();
    if (version === "") version = "latest";

    // See if this version is affected by version substitutions
    for (let i = 0; i < MongoRunner.binVersionSubs.length; i++) {
        let sub = MongoRunner.binVersionSubs[i];
        if (sub.pattern == version) {
            return sub.version;
        }
    }

    return version;
};

/**
 * Returns true if two version strings could represent the same version. This is true
 * if, after passing the versions through getBinVersionFor, the versions have the
 * same value for each version component up through the length of the shorter version.
 *
 * That is, 3.2.4 compares equal to 3.2, but 3.2.4 does not compare equal to 3.2.3.
 */
MongoRunner.areBinVersionsTheSame = function (versionA, versionB) {
    // Check for invalid version strings first.
    convertVersionStringToArray(MongoRunner.getBinVersionFor(versionA));
    convertVersionStringToArray(MongoRunner.getBinVersionFor(versionB));
    try {
        return 0 === MongoRunner.compareBinVersions(versionA, versionB);
    } catch (err) {
        // compareBinVersions() throws an error if two versions differ only by the git hash.
        return false;
    }
};

/**
 * Compares two version strings and returns:
 *      1, if the first is more recent
 *      0, if they are equal
 *     -1, if the first is older
 *
 * Note that this function only compares up to the length of the shorter version.
 * Because of this, minor versions will compare equal to the major versions they stem
 * from, but major-major and minor-minor version pairs will undergo strict comparison.
 */
MongoRunner.compareBinVersions = function (versionA, versionB) {
    let stringA = versionA;
    let stringB = versionB;

    versionA = convertVersionStringToArray(MongoRunner.getBinVersionFor(versionA));
    versionB = convertVersionStringToArray(MongoRunner.getBinVersionFor(versionB));

    // Treat the githash as a separate element, if it's present.
    versionA.push(...versionA.pop().split("-"));
    versionB.push(...versionB.pop().split("-"));

    let elementsToCompare = Math.min(versionA.length, versionB.length);

    for (let i = 0; i < elementsToCompare; ++i) {
        let elementA = versionA[i];
        let elementB = versionB[i];

        if (elementA === elementB) {
            continue;
        }

        let numA = parseInt(elementA);
        let numB = parseInt(elementB);

        assert(!isNaN(numA) && !isNaN(numB), `Cannot compare non-equal non-numeric versions. ${elementA}, ${elementB}`);

        if (numA > numB) {
            return 1;
        } else if (numA < numB) {
            return -1;
        }

        assert(false, `Unreachable case. Provided versions: {${stringA}, ${stringB}}`);
    }

    return 0;
};

MongoRunner.logicalOptions = {
    runId: true,
    env: true,
    pathOpts: true,
    remember: true,
    noRemember: true,
    appendOptions: true,
    restart: true,
    noCleanData: true,
    cleanData: true,
    startClean: true,
    forceLock: true,
    useLogFiles: true,
    logFile: true,
    useHostName: true,
    useHostname: true,
    noReplSet: true,
    forgetPort: true,
    arbiter: true,
    binVersion: true,
    waitForConnect: true,
    waitForConnectTimeoutMS: true,
    bridgeOptions: true,
    skipValidation: true,
    backupOnRestartDir: true,
    allowedExitCode: true,
};

MongoRunner.toRealPath = function (path, pathOpts) {
    // Replace all $pathOptions with actual values
    pathOpts ||= {};
    path = path.replace(/\$dataPath/g, MongoRunner.dataPath);
    path = path.replace(/\$dataDir/g, MongoRunner.dataDir);
    for (let key in pathOpts) {
        path = path.replace(RegExp("\\$" + RegExp.escape(key), "g"), pathOpts[key]);
    }

    // Relative path
    // Detect Unix and Windows absolute paths
    // as well as Windows drive letters
    // Also captures Windows UNC paths

    if (!path.match(/^(\/|\\|[A-Za-z]:)/)) {
        if (path != "" && !path.endsWith("/")) path += "/";

        path = MongoRunner.dataPath + path;
    }

    return path;
};

MongoRunner.toRealDir = function (path, pathOpts) {
    path = MongoRunner.toRealPath(path, pathOpts);

    if (path.endsWith("/")) path = path.substring(0, path.length - 1);

    return path;
};

MongoRunner.toRealFile = MongoRunner.toRealDir;

/**
 * Returns an iterator object which yields successive versions on calls to advance(), starting
 * from a random initial position, from an array of versions.
 *
 * If passed a single version string or an already-existing version iterator, just returns the
 * object itself, since it will yield correctly on calls to advance().
 *
 * @param {Array.<String>}|{String}|{versionIterator}
 */
MongoRunner.versionIterator = function (arr, isRandom) {
    // If this isn't an array of versions, or is already an iterator, just use it
    if (typeof arr == "string") return arr;
    if (arr.isVersionIterator) return arr;

    if (isRandom == undefined) isRandom = false;

    // Starting pos
    let i = isRandom ? parseInt(Random.rand() * arr.length) : 0;

    return new MongoRunner.versionIterator.iterator(i, arr);
};

MongoRunner.versionIterator.iterator = function (i, arr) {
    if (!Array.isArray(arr)) {
        throw new Error("Expected an array for the second argument, but got: " + tojson(arr));
    }

    this.current = function current() {
        return arr[i];
    };

    // We define the toString() method as an alias for current() so that concatenating a version
    // iterator with a string returns the next version in the list without introducing any
    // side-effects.
    this.toString = this.current;

    this.advance = function advance() {
        i = (i + 1) % arr.length;
    };

    this.isVersionIterator = true;
};

/**
 * Converts the args object by pairing all keys with their value and appending
 * dash-dash (--) to the keys. The only exception to this rule are keys that
 * are defined in MongoRunner.logicalOptions, of which they will be ignored.
 *
 * @param {string} binaryName
 * @param {Object} args
 *
 * @return {Array.<String>} an array of parameter strings that can be passed
 *   to the binary.
 */
MongoRunner.arrOptions = function (binaryName, args) {
    let fullArgs = [""];

    // isObject returns true even if "args" is an array, so the else branch of this statement is
    // dead code.  See SERVER-14220.
    if (isObject(args) || (args.length == 1 && isObject(args[0]))) {
        let o = isObject(args) ? args : args[0];

        // If we've specified a particular binary version, use that
        if (o.binVersion && o.binVersion != "" && o.binVersion != shellVersion()) {
            binaryName += "-" + o.binVersion;
        } else {
            binaryName = getMongoSuffixPath(binaryName);
        }

        // Manage legacy options
        let isValidOptionForBinary = function (option, value) {
            if (!o.binVersion) return true;

            return true;
        };

        let addOptionsToFullArgs = function (k, v) {
            if (v === undefined || v === null) return;

            fullArgs.push("--" + k);

            if (v !== "") {
                fullArgs.push("" + v);
            }
        };

        for (let k in o) {
            // Make sure our logical option should be added to the array of options
            if (!o.hasOwnProperty(k) || k in MongoRunner.logicalOptions || !isValidOptionForBinary(k, o[k])) continue;

            if ((k == "v" || k == "verbose") && isNumber(o[k])) {
                let n = o[k];
                if (n > 0) {
                    if (n > 10) n = 10;
                    let temp = "-";
                    while (n-- > 0) temp += "v";
                    fullArgs.push(temp);
                }
            } else if (k === "setParameter" && isObject(o[k])) {
                // If the value associated with the setParameter option is an object, we want
                // to add all key-value pairs in that object as separate --setParameters.
                Object.keys(o[k]).forEach(function (paramKey) {
                    let value = o[k][paramKey];
                    if (isObject(value) && !Array.isArray(value)) {
                        value = JSON.stringify(value);
                    }
                    addOptionsToFullArgs(k, "" + paramKey + "=" + value);
                });
            } else {
                addOptionsToFullArgs(k, o[k]);
            }
        }
    } else {
        for (let i = 0; i < args.length; i++) fullArgs.push(args[i]);
    }

    fullArgs[0] = binaryName;
    return fullArgs;
};

MongoRunner.arrToOpts = function (arr) {
    let opts = {};
    for (let i = 1; i < arr.length; i++) {
        if (arr[i].startsWith("-")) {
            let opt = arr[i].replace(/^-/, "").replace(/^-/, "");

            if (arr.length > i + 1 && !arr[i + 1].startsWith("-")) {
                opts[opt] = arr[i + 1];
                i++;
            } else {
                opts[opt] = "";
            }

            if (opt.replace(/v/g, "") == "") {
                opts["verbose"] = opt.length;
            }
        }
    }

    return opts;
};

MongoRunner.savedOptions = {};

MongoRunner.mongoOptions = function (opts) {
    // Don't remember waitForConnect
    let waitForConnect = opts.waitForConnect;
    delete opts.waitForConnect;

    // If we're a mongo object
    if (opts.getDB) {
        opts = {restart: opts.runId};
    }

    // Initialize and create a copy of the opts
    opts = Object.merge(opts || {}, {});

    if (!opts.restart) opts.restart = false;

    // RunId can come from a number of places
    // If restart is passed as an old connection
    if (opts.restart && opts.restart.getDB) {
        opts.runId = opts.restart.runId;
        opts.restart = true;
    }
    // If it's the runId itself
    else if (isObject(opts.restart)) {
        opts.runId = opts.restart;
        opts.restart = true;
    }

    if (isObject(opts.remember)) {
        opts.runId = opts.remember;
        opts.remember = true;
    } else if (opts.remember == undefined) {
        // Remember by default if we're restarting
        opts.remember = opts.restart;
    }

    // If we passed in restart : <conn> or runId : <conn>
    if (isObject(opts.runId) && opts.runId.runId) opts.runId = opts.runId.runId;

    if (opts.restart && opts.remember) {
        opts = Object.merge(MongoRunner.savedOptions[opts.runId], opts);
    }

    // Create a new runId
    opts.runId ||= ObjectId();

    if (opts.forgetPort) {
        delete opts.port;
    }

    // Normalize and get the binary version to use
    if (opts.hasOwnProperty("binVersion")) {
        if (opts.binVersion instanceof MongoRunner.versionIterator.iterator) {
            // Advance the version iterator so that subsequent calls to
            // MongoRunner.mongoOptions() use the next version in the list.
            const iterator = opts.binVersion;
            opts.binVersion = iterator.current();
            iterator.advance();
        }
        opts.binVersion = MongoRunner.getBinVersionFor(opts.binVersion);
    }

    // Default for waitForConnect is true
    opts.waitForConnect = waitForConnect == undefined || waitForConnect == null ? true : waitForConnect;

    opts.port ||= allocatePort();

    if (jsTestOptions().tlsMode && !opts.tlsMode) {
        opts.tlsMode = jsTestOptions().tlsMode;
        if (opts.tlsMode != "disabled") {
            opts.tlsAllowInvalidHostnames = "";
            if (!jsTestOptions().shellTlsCertificateKeyFile) {
                opts.tlsAllowConnectionsWithoutCertificates = "";
            }
        }
    }
    if (jsTestOptions().tlsCAFile && !opts.tlsCAFile) {
        opts.tlsCAFile = jsTestOptions().tlsCAFile;
    }

    const setParameters = jsTestOptions().setParameters || {};
    const tlsEnabled = (opts.tlsMode && opts.tlsMode != "disabled") || (opts.sslMode && opts.sslMode != "disabled");
    if (setParameters.featureFlagGRPC && tlsEnabled) {
        opts.grpcPort ||= allocatePort();
        grpcToMongoRpcPortMap[opts.grpcPort] = opts.port;
    }

    opts.pathOpts = Object.merge(opts.pathOpts || {}, {port: "" + opts.port, runId: "" + opts.runId});

    let shouldRemember = (!opts.restart && !opts.noRemember) || (opts.restart && opts.appendOptions);
    if (shouldRemember) {
        MongoRunner.savedOptions[opts.runId] = Object.merge(opts, {});
        // We don't want to persist 'waitForConnect' across node restarts.
        delete MongoRunner.savedOptions[opts.runId].waitForConnect;
    }

    if (jsTestOptions().networkMessageCompressors) {
        opts.networkMessageCompressors = jsTestOptions().networkMessageCompressors;
    }

    if (!opts.hasOwnProperty("bind_ip")) {
        opts.bind_ip = "0.0.0.0";
    }

    return opts;
};

// Returns an array of integers representing the version provided.
// Ex: "3.3.12" => [3, 3, 12]
function _convertVersionToIntegerArray(version) {
    let versionParts = convertVersionStringToArray(version)
        .slice(0, 3)
        .map((part) => parseInt(part, 10));
    if (versionParts.length === 2) {
        versionParts.push(Infinity);
    }
    return versionParts;
}

// Returns if version2 is equal to, or came after, version 1.
let _isMongodVersionEqualOrAfter = function (version1, version2) {
    if (version2 === "latest") {
        return true;
    }

    let versionParts1 = _convertVersionToIntegerArray(version1);
    let versionParts2 = _convertVersionToIntegerArray(version2);
    if (
        versionParts2[0] > versionParts1[0] ||
        (versionParts2[0] === versionParts1[0] && versionParts2[1] > versionParts1[1]) ||
        (versionParts2[0] === versionParts1[0] &&
            versionParts2[1] === versionParts1[1] &&
            versionParts2[2] >= versionParts1[2])
    ) {
        return true;
    }

    return false;
};

// Removes a setParameter parameter from mongods or mongoses running a version that won't recognize
// them.
let _removeSetParameterIfBeforeVersion = function (opts, parameterName, requiredVersion, isMongos = false) {
    let processString = isMongos ? "mongos" : "mongod";
    let versionCompatible =
        opts.binVersion === "" ||
        opts.binVersion === undefined ||
        _isMongodVersionEqualOrAfter(requiredVersion, opts.binVersion);
    if (!versionCompatible && opts.setParameter && opts.setParameter[parameterName] != undefined) {
        print(
            "Removing '" +
                parameterName +
                "' setParameter with value " +
                opts.setParameter[parameterName] +
                " because it isn't compatible with " +
                processString +
                " running version " +
                opts.binVersion,
        );
        delete opts.setParameter[parameterName];
    }
};

/**
 * @option {object} opts
 *
 *   {
 *     dbpath {string}
 *     useLogFiles {boolean}: use with logFile option.
 *     logFile {string}: path to the log file. If not specified and useLogFiles
 *       is true, automatically creates a log file inside dbpath.
 *     keyFile
 *     replSet
 *     oplogSize
 *   }
 */
MongoRunner.mongodOptions = function (opts = {}) {
    opts = MongoRunner.mongoOptions(opts);

    if (jsTestOptions().alwaysUseLogFiles) {
        if (opts.cleanData || opts.startClean || opts.noCleanData === false || opts.useLogFiles === false) {
            throw new Error("Always using log files, but received conflicting option.");
        }

        opts.cleanData = false;
        opts.startClean = false;
        opts.noCleanData = true;
        opts.useLogFiles = true;
        opts.logappend = "";
    }

    opts.dbpath = MongoRunner.toRealDir(opts.dbpath || "$dataDir/mongod-$port", opts.pathOpts);

    opts.pathOpts = Object.merge(opts.pathOpts, {dbpath: opts.dbpath});

    opts.setParameter ||= {};
    if (jsTestOptions().enableTestCommands && typeof opts.setParameter !== "string") {
        if (
            jsTestOptions().setParameters &&
            jsTestOptions().setParameters.disableTransitionFromLatestToLastContinuous
        ) {
            opts.setParameter["disableTransitionFromLatestToLastContinuous"] =
                jsTestOptions().setParameters.disableTransitionFromLatestToLastContinuous;
        } else {
            opts.setParameter["disableTransitionFromLatestToLastContinuous"] = false;
        }
    }

    if (jsTestOptions().mongodTlsCertificateKeyFile && !opts.tlsCertificateKeyFile) {
        opts.tlsCertificateKeyFile = jsTestOptions().mongodTlsCertificateKeyFile;
    }

    _removeSetParameterIfBeforeVersion(opts, "writePeriodicNoops", "3.3.12");
    _removeSetParameterIfBeforeVersion(opts, "numInitialSyncAttempts", "3.3.12");
    _removeSetParameterIfBeforeVersion(opts, "numInitialSyncConnectAttempts", "3.3.12");
    _removeSetParameterIfBeforeVersion(opts, "migrationLockAcquisitionMaxWaitMS", "4.1.7");
    _removeSetParameterIfBeforeVersion(opts, "shutdownTimeoutMillisForSignaledShutdown", "4.5.0");
    _removeSetParameterIfBeforeVersion(opts, "failpoint.PrimaryOnlyServiceSkipRebuildingInstances", "4.8.0");
    _removeSetParameterIfBeforeVersion(opts, "enableDefaultWriteConcernUpdatesForInitiate", "5.0.0");
    _removeSetParameterIfBeforeVersion(opts, "enableReconfigRollbackCommittedWritesCheck", "5.0.0");
    _removeSetParameterIfBeforeVersion(opts, "allowMultipleArbiters", "5.3.0");
    _removeSetParameterIfBeforeVersion(opts, "internalQueryDisableExclusionProjectionFastPath", "6.2.0");
    _removeSetParameterIfBeforeVersion(opts, "disableTransitionFromLatestToLastContinuous", "7.0.0");
    _removeSetParameterIfBeforeVersion(opts, "defaultConfigCommandTimeoutMS", "7.3.0");
    _removeSetParameterIfBeforeVersion(opts, "enableAutoCompaction", "7.3.0");

    if (!opts.logFile && opts.useLogFiles) {
        opts.logFile = opts.dbpath + "/mongod.log";
    } else if (opts.logFile) {
        opts.logFile = MongoRunner.toRealFile(opts.logFile, opts.pathOpts);
    }

    if (opts.logFile !== undefined) {
        opts.logpath = opts.logFile;
    }

    if (jsTestOptions().keyFile && !opts.keyFile) {
        opts.keyFile = jsTestOptions().keyFile;
    }

    if (opts.hasOwnProperty("enableEncryption")) {
        // opts.enableEncryption, if set, must be an empty string
        if (opts.enableEncryption !== "") {
            throw new Error("The enableEncryption option must be an empty string if it is " + "specified");
        }
    } else if (jsTestOptions().enableEncryption !== undefined) {
        if (jsTestOptions().enableEncryption !== "") {
            throw new Error("The enableEncryption option must be an empty string if it is " + "specified");
        }
        opts.enableEncryption = "";
    }

    if (opts.hasOwnProperty("encryptionCipherMode")) {
        if (typeof opts.encryptionCipherMode !== "string") {
            // opts.encryptionCipherMode, if set, must be a string
            throw new Error("The encryptionCipherMode option must be a string if it is specified");
        }
    } else if (jsTestOptions().encryptionCipherMode !== undefined) {
        if (typeof jsTestOptions().encryptionCipherMode !== "string") {
            throw new Error("The encryptionCipherMode option must be a string if it is specified");
        }
        opts.encryptionCipherMode = jsTestOptions().encryptionCipherMode;
    }

    if (opts.hasOwnProperty("encryptionKeyFile")) {
        // opts.encryptionKeyFile, if set, must be a string
        if (typeof opts.encryptionKeyFile !== "string") {
            throw new Error("The encryptionKeyFile option must be a string if it is specified");
        }
    } else if (jsTestOptions().encryptionKeyFile !== undefined) {
        if (typeof jsTestOptions().encryptionKeyFile !== "string") {
            throw new Error("The encryptionKeyFile option must be a string if it is specified");
        }
        opts.encryptionKeyFile = jsTestOptions().encryptionKeyFile;
    }

    if (opts.hasOwnProperty("auditDestination")) {
        // opts.auditDestination, if set, must be a string
        if (typeof opts.auditDestination !== "string") {
            throw new Error("The auditDestination option must be a string if it is specified");
        }
    } else if (jsTestOptions().auditDestination !== undefined) {
        if (typeof jsTestOptions().auditDestination !== "string") {
            throw new Error("The auditDestination option must be a string if it is specified");
        }
        opts.auditDestination = jsTestOptions().auditDestination;
    }

    if (opts.hasOwnProperty("auditPath")) {
        // We need to reformat the auditPath to include the proper port
        opts.auditPath = MongoRunner.toRealPath(opts.auditPath, opts);
    }

    if (opts.hasOwnProperty("setParameter")) {
        if (
            (typeof opts.setParameter === "string" &&
                opts.setParameter.includes("multitenancySupport") &&
                opts.setParameter.includes("true")) ||
            opts.setParameter.multitenancySupport
        )
            opts.noscripting = "";
    }

    if (opts.noReplSet) opts.replSet = null;
    if (opts.arbiter) opts.oplogSize = 1;

    return opts;
};

MongoRunner.mongosOptions = function (opts) {
    opts = MongoRunner.mongoOptions(opts);

    // Normalize configdb option to be host string if currently a host
    if (opts.configdb && opts.configdb.getDB) {
        opts.configdb = opts.configdb.host;
    }

    if (jsTestOptions().alwaysUseLogFiles) {
        if (opts.useLogFiles === false) {
            throw new Error("Always using log files, but received conflicting option.");
        }

        opts.useLogFiles = true;
        opts.logappend = "";
    }

    opts.pathOpts = Object.merge(opts.pathOpts, {configdb: opts.configdb.replace(/:|\/|,/g, "-")});

    if (!opts.logFile && opts.useLogFiles) {
        opts.logFile = MongoRunner.toRealFile("$dataDir/mongos-$configdb-$port.log", opts.pathOpts);
    } else if (opts.logFile) {
        opts.logFile = MongoRunner.toRealFile(opts.logFile, opts.pathOpts);
    }

    if (opts.logFile !== undefined) {
        opts.logpath = opts.logFile;
    }

    let testOptions = jsTestOptions();
    if (testOptions.keyFile && !opts.keyFile) {
        opts.keyFile = testOptions.keyFile;
    }

    if (testOptions.mongosTlsCertificateKeyFile && !opts.tlsCertificateKeyFile) {
        opts.tlsCertificateKeyFile = testOptions.mongosTlsCertificateKeyFile;
    }

    if (opts.hasOwnProperty("auditDestination")) {
        // opts.auditDestination, if set, must be a string
        if (typeof opts.auditDestination !== "string") {
            throw new Error("The auditDestination option must be a string if it is specified");
        }
    } else if (testOptions.auditDestination !== undefined) {
        if (typeof testOptions.auditDestination !== "string") {
            throw new Error("The auditDestination option must be a string if it is specified");
        }
        opts.auditDestination = testOptions.auditDestination;
    }

    if (!opts.hasOwnProperty("binVersion") && testOptions.mongosBinVersion) {
        opts.binVersion = MongoRunner.getBinVersionFor(testOptions.mongosBinVersion);
    }

    if (opts.hasOwnProperty("auditPath")) {
        // We need to reformat the auditPath to include the proper port
        opts.auditPath = MongoRunner.toRealPath(opts.auditPath, opts);
    }

    _removeSetParameterIfBeforeVersion(opts, "mongosShutdownTimeoutMillisForSignaledShutdown", "4.5.0", true);
    _removeSetParameterIfBeforeVersion(opts, "failpoint.skipClusterParameterRefresh", "7.1.0", true);
    _removeSetParameterIfBeforeVersion(opts, "defaultConfigCommandTimeoutMS", "7.3.0", true);

    return opts;
};

/**
 * @return {NumberLong[]} Running pids e.g. those started by `MongoRunner.runMongod`.
 */
MongoRunner.runningChildPids = function () {
    return _runningMongoChildProcessIds();
};

/**
 * Starts a mongod instance.
 *
 * @param {Object} [opts]
 *
 *   {
 *     useHostName {boolean}: Uses hostname of machine if true.
 *     forceLock {boolean}: Deletes the lock file if set to true.
 *     dbpath {string}: location of db files.
 *     cleanData {boolean}: Removes all files in dbpath if true.
 *     startClean {boolean}: same as cleanData.
 *     noCleanData {boolean}: Do not clean files (cleanData takes priority).
 *     binVersion {string}: version for binary (also see MongoRunner.binVersionSubs).
 *
 *     @see MongoRunner.mongodOptions for other options
 *   }
 *
 * @return {Mongo} connection object to the started mongod instance.
 *
 * @see MongoRunner.arrOptions
 */
MongoRunner.runMongod = function (opts) {
    opts ||= {};
    let env = undefined;
    let useHostName = true;
    let runId = null;
    let waitForConnect = true;
    let waitForConnectTimeoutMS = undefined;
    let fullOptions = opts;

    if (isObject(opts)) {
        opts = MongoRunner.mongodOptions(opts);
        fullOptions = opts;

        if (opts.useHostName != undefined) {
            useHostName = opts.useHostName;
        } else if (opts.useHostname != undefined) {
            useHostName = opts.useHostname;
        } else {
            useHostName = true; // Default to true
        }
        env = opts.env;
        runId = opts.runId;
        waitForConnect = opts.waitForConnect;
        waitForConnectTimeoutMS = opts.waitForConnectTimeoutMS;

        let backupOnRestartDir = jsTest.options()["backupOnRestartDir"] || false;

        if (opts.forceLock) removeFile(opts.dbpath + "/mongod.lock");
        if (opts.cleanData || opts.startClean || (!opts.restart && !opts.noCleanData) || !pathExists(opts.dbpath)) {
            print("Resetting db path '" + opts.dbpath + "'");
            resetDbpath(opts.dbpath);
        } else {
            if (backupOnRestartDir) {
                let pathOpts = {"backupDir": backupOnRestartDir, "dbpath": opts.dbpath};
                let backupDir = MongoRunner.toRealDir("$backupDir/$dbpath", pathOpts);
                // `toRealDir` assumes the patterned directory should be under
                // `MongoRunner.dataPath`. In this case, preserve the user input as is.
                backupDir = backupDir.substring(MongoRunner.dataPath.length);

                print("Backing up data files. DBPath: " + opts.dbpath + " Backing up under: " + backupDir);
                copyDbpath(opts.dbpath, backupDir);
            }
        }

        opts = MongoRunner.arrOptions("mongod", opts);
    }

    let mongod = MongoRunner._startWithArgs(opts, env, waitForConnect, waitForConnectTimeoutMS);
    if (!mongod) {
        return null;
    }

    mongod.commandLine = MongoRunner.arrToOpts(opts);
    let connectPort = jsTestOptions().shellGRPC ? mongod.commandLine.grpcPort : mongod.commandLine.port;
    mongod.hostNoPort = useHostName ? getHostName() : "localhost";
    mongod.name = mongod.hostNoPort + ":" + mongod.commandLine.port;
    mongod.host = mongod.hostNoPort + ":" + connectPort;
    mongod.port = parseInt(connectPort);
    mongod.runId = runId || ObjectId();
    mongod.dbpath = fullOptions.dbpath;
    mongod.savedOptions = MongoRunner.savedOptions[mongod.runId];
    mongod.fullOptions = fullOptions;

    return mongod;
};

MongoRunner.getMongosName = function (port, useHostName) {
    return (useHostName ? getHostName() : "localhost") + ":" + port;
};

MongoRunner.runMongos = function (opts) {
    opts ||= {};

    let env = undefined;
    let useHostName = false;
    let runId = null;
    let waitForConnect = true;
    let waitForConnectTimeoutMS = undefined;
    let fullOptions = opts;

    if (isObject(opts)) {
        opts = MongoRunner.mongosOptions(opts);
        fullOptions = opts;

        useHostName = opts.useHostName || opts.useHostname;
        runId = opts.runId;
        waitForConnect = opts.waitForConnect;
        waitForConnectTimeoutMS = opts.waitForConnectTimeoutMS;
        env = opts.env;
        opts = MongoRunner.arrOptions("mongos", opts);
    }

    let mongos = MongoRunner._startWithArgs(opts, env, waitForConnect, waitForConnectTimeoutMS);
    if (!mongos) {
        return null;
    }

    mongos.commandLine = MongoRunner.arrToOpts(opts);
    let connectPort = jsTestOptions().shellGRPC ? mongos.commandLine.grpcPort : mongos.commandLine.port;
    mongos.name = MongoRunner.getMongosName(mongos.commandLine.port, useHostName);
    mongos.host = MongoRunner.getMongosName(connectPort, useHostName);
    mongos.port = parseInt(connectPort);
    mongos.runId = runId || ObjectId();
    mongos.savedOptions = MongoRunner.savedOptions[mongos.runId];
    mongos.fullOptions = fullOptions;

    return mongos;
};

MongoRunner.StopError = function (returnCode) {
    this.name = "StopError";
    this.returnCode = returnCode;
    this.message = "MongoDB process stopped with exit code: " + this.returnCode;
    this.stack = this.toString() + "\n" + new Error().stack;
};

MongoRunner.StopError.prototype = Object.create(Error.prototype);
MongoRunner.StopError.prototype.constructor = MongoRunner.StopError;

// Constants for exit codes of MongoDB processes
// On Windows, std::abort causes the process to exit with return code 14.
MongoRunner.EXIT_ABORT = _isWindows() ? 14 : 6;
MongoRunner.EXIT_CLEAN = 0;
MongoRunner.EXIT_BADOPTIONS = 2;
MongoRunner.EXIT_REPLICATION_ERROR = 3;
MongoRunner.EXIT_NEED_UPGRADE = 4;
MongoRunner.EXIT_SHARDING_ERROR = 5;
// SIGKILL is translated to TerminateProcess() on Windows, which causes the program to
// terminate with exit code 1.
MongoRunner.EXIT_SIGKILL = _isWindows() ? 1 : 9;
MongoRunner.EXIT_KILL = 12;
MongoRunner.EXIT_ABRUPT = 14;
MongoRunner.EXIT_NTSERVICE_ERROR = 20;
MongoRunner.EXIT_JAVA = 21;
MongoRunner.EXIT_OOM_MALLOC = 42;
MongoRunner.EXIT_OOM_REALLOC = 43;
MongoRunner.EXIT_FS = 45;
MongoRunner.EXIT_CLOCK_SKEW = 47; // OpTime clock skew; deprecated
MongoRunner.EXIT_NET_ERROR = 48;
MongoRunner.EXIT_WINDOWS_SERVICE_STOP = 49;
MongoRunner.EXIT_POSSIBLE_CORRUPTION = 60;
MongoRunner.EXIT_NEED_DOWNGRADE = 62;
MongoRunner.EXIT_UNCAUGHT = 100; // top level exception that wasn't caught
MongoRunner.EXIT_TEST = 101;
MongoRunner.EXIT_AUDIT_ROTATE_ERROR = 102;

MongoRunner.validateCollectionsCallback = function (port, options) {};

/**
 * Kills a mongod process.
 *
 * @param {Mongo} conn the connection object to the process to kill
 * @param {number} signal The signal number to use for killing
 * @param {Object} opts Additional options. Format:
 *    {
 *      auth: {
 *        user {string}: admin user name
 *        pwd {string}: admin password
 *      },
 *      skipValidation: <bool>,
 *      skipValidatingExitCode: <bool>,
 *      allowedExitCode: <int>
 *    }
 * @param {boolean} waitpid should we wait for the process to terminate after stopping it.
 *
 * Note: The auth option is required in a authenticated mongod running in Windows since
 *  it uses the shutdown command, which requires admin credentials.
 */
let stopMongoProgram = function (conn, signal, opts, waitpid) {
    if (!conn.pid) {
        throw new Error(
            "first arg must have a `pid` property; " + "it is usually the object returned from MongoRunner.runMongod/s",
        );
    }

    if (!conn.port) {
        throw new Error(
            "first arg must have a `port` property; " +
                "it is usually the object returned from MongoRunner.runMongod/s",
        );
    }

    signal = parseInt(signal) || SIGTERM;
    opts ||= {};
    waitpid = waitpid === undefined ? true : waitpid;

    // If we are executing an unclean shutdown, we want to avoid checking collection counts during
    // validation, since the counts may be inaccurate.
    if (signal !== SIGTERM && typeof TestData !== "undefined") {
        TestData.skipEnforceFastCountOnValidate = true;
    }

    const allowedExitCode = opts.allowedExitCode ? opts.allowedExitCode : MongoRunner.EXIT_CLEAN;
    if (!waitpid && allowedExitCode !== MongoRunner.EXIT_CLEAN) {
        throw new Error("Must wait for process to exit if it is expected to exit uncleanly");
    }

    let port = parseInt(conn.port);

    if (grpcToMongoRpcPortMap[port]) {
        port = grpcToMongoRpcPortMap[port];
    }

    // If the return code is in the serverExitCodeMap, it means the server crashed on startup.
    // We just use the recorded return code instead of stopping the program.
    let returnCode;
    if (serverExitCodeMap.hasOwnProperty(port)) {
        returnCode = serverExitCodeMap[port];
        delete serverExitCodeMap[port];
    } else {
        // Invoke callback to validate collections and indexes before shutting down mongod.
        // We skip calling the callback function when the expected return code of
        // the mongod process is non-zero since it's likely the process has already exited.

        let skipValidation = false;
        if (opts.skipValidation) {
            skipValidation = true;
        }

        if (allowedExitCode === MongoRunner.EXIT_CLEAN && !skipValidation) {
            MongoRunner.validateCollectionsCallback(port);
        }

        returnCode = _stopMongoProgram(port, signal, opts, waitpid);
    }

    // If we are not waiting for shutdown, then there is no exit code to check.
    if (!waitpid) {
        returnCode = 0;
    }
    if (allowedExitCode !== returnCode && !opts.skipValidatingExitCode) {
        throw new MongoRunner.StopError(returnCode);
    } else if (returnCode !== MongoRunner.EXIT_CLEAN) {
        print("MongoDB process on port " + port + " intentionally exited with error code ", returnCode);
    }

    return returnCode;
};

MongoRunner.stopMongod = stopMongoProgram;
MongoRunner.stopMongos = stopMongoProgram;

// Given a test name figures out a directory for that test to use for dump files and makes sure
// that directory exists and is empty.
MongoRunner.getAndPrepareDumpDirectory = function (testName) {
    let dir = MongoRunner.dataPath + testName + "_external/";
    resetDbpath(dir);
    return dir;
};

/**
 * Returns a new argArray with any test-specific arguments added.
 */
function appendSetParameterArgs(argArray) {
    function argArrayContains(key) {
        return (
            argArray.filter((val) => {
                return typeof val === "string" && val.indexOf(key) === 0;
            }).length > 0
        );
    }

    function argArrayContainsSetParameterValue(value) {
        assert(value.endsWith("="), "Expected value argument to be of the form <parameterName>=");
        return argArray.some(function (el) {
            return typeof el === "string" && el.startsWith(value);
        });
    }

    // programName includes the version, e.g., mongod-3.2.
    // baseProgramName is the program name without any version information, e.g., mongod.
    let programName = argArray[0];
    const separator = _isWindows() ? "\\" : "/";
    if (programName.indexOf(separator) !== -1) {
        let pathElements = programName.split(separator);
        programName = pathElements[pathElements.length - 1];
    }

    let [baseProgramName, programVersion] = programName.split("-");

    // Setting programMajorMinorVersion to the maximum value for the latest binary version
    // simplifies version checks below.
    const lastestMajorMinorVersion = Number.MAX_SAFE_INTEGER;
    const lastContinuousVersion = convertVersionStringToInteger(MongoRunner.getBinVersionFor("last-continuous"));
    const lastLTSVersion = convertVersionStringToInteger(MongoRunner.getBinVersionFor("last-lts"));
    let programMajorMinorVersion = lastestMajorMinorVersion;

    if (programVersion) {
        programMajorMinorVersion = convertVersionStringToInteger(programVersion);
    }

    if (baseProgramName === "mongod" || baseProgramName === "mongos") {
        if (jsTest.options().enableTestCommands) {
            argArray.push(...["--setParameter", "enableTestCommands=1"]);
        }

        if (programMajorMinorVersion >= 440) {
            if (jsTest.options().testingDiagnosticsEnabled) {
                argArray.push(...["--setParameter", "testingDiagnosticsEnabled=1"]);
            }
        }

        if (jsTest.options().authMechanism && jsTest.options().authMechanism != "SCRAM-SHA-256") {
            if (!argArrayContainsSetParameterValue("authenticationMechanisms=")) {
                argArray.push(...["--setParameter", "authenticationMechanisms=" + jsTest.options().authMechanism]);
            }
        }
        if (jsTest.options().auth) {
            argArray.push(...["--setParameter", "enableLocalhostAuthBypass=false"]);
        }

        // New options in 3.5.x
        if (programMajorMinorVersion >= 350) {
            // Disable background cache refreshing to avoid races in tests
            if (!argArrayContainsSetParameterValue("disableLogicalSessionCacheRefresh=")) {
                argArray.push(...["--setParameter", "disableLogicalSessionCacheRefresh=true"]);
            }
        }

        // Since options may not be backward compatible, mongos options are not
        // set on older versions, e.g., mongos-3.0.
        if (baseProgramName === "mongos" && programMajorMinorVersion == lastestMajorMinorVersion) {
            // apply setParameters for mongos
            if (jsTest.options().setParametersMongos) {
                let params = jsTest.options().setParametersMongos;
                for (let paramName of Object.keys(params)) {
                    if (argArrayContainsSetParameterValue(paramName + "=")) {
                        continue;
                    }

                    const paramVal = ((param) => {
                        if (typeof param === "object") {
                            return JSON.stringify(param);
                        }

                        return param;
                    })(params[paramName]);
                    const setParamStr = paramName + "=" + paramVal;
                    argArray.push(...["--setParameter", setParamStr]);
                }
            }
        } else if (baseProgramName === "mongod") {
            if (jsTestOptions().roleGraphInvalidationIsFatal) {
                argArray.push(...["--setParameter", "roleGraphInvalidationIsFatal=true"]);
            }

            // Set storageEngine for mongod. There was no storageEngine parameter before 3.0.
            if (jsTest.options().storageEngine && programMajorMinorVersion >= 300) {
                if (!argArrayContains("--storageEngine")) {
                    argArray.push(...["--storageEngine", jsTest.options().storageEngine]);
                }
            }

            function isSetParameterMentioned(setParameters, key) {
                if (setParameters !== undefined && setParameters[key] !== undefined) {
                    return true;
                }

                if (argArrayContainsSetParameterValue(key + "=")) {
                    return true;
                }

                return false;
            }

            // The 'logComponentVerbosity' parameter must be passed in on the last continuous
            // version and last LTS version as well.
            if (
                (programMajorMinorVersion === lastContinuousVersion || programMajorMinorVersion === lastLTSVersion) &&
                isSetParameterMentioned(jsTest.options().setParameters, "logComponentVerbosity") &&
                !argArrayContainsSetParameterValue("logComponentVerbosity=")
            ) {
                let logVerbosityParam = jsTest.options().setParameters["logComponentVerbosity"];
                if (typeof logVerbosityParam === "object") {
                    logVerbosityParam = JSON.stringify(logVerbosityParam);
                }
                argArray.push(...["--setParameter", "logComponentVerbosity=" + logVerbosityParam]);
            }

            if (programMajorMinorVersion >= 530 && !argArrayContainsSetParameterValue("backtraceLogFile=")) {
                let randomName = "";
                let randomStrLen = 20;
                const chars = "qwertyuiopasdfghjklzxcvbnm1234567890";
                for (let i = 0; i <= randomStrLen; i++) {
                    randomName += chars[(Math.random() * 1000) % chars.length ^ 0];
                }
                const backtraceLogFilePath = MongoRunner.dataDir + "/" + randomName + Date.now() + ".stacktrace";
                argArray.push(...["--setParameter", "backtraceLogFile=" + backtraceLogFilePath]);
            }

            // We are choosing an arbitrary jsTestOptions value to test that we are running as a
            // part of a test, and not via some other method. We are assuming if someone is running
            // MongoRunner.runMongod via the command line directly that this value would not be set.
            if (
                jsTestOptions().hasOwnProperty("auth") &&
                (programMajorMinorVersion >= 800 ||
                    (programMajorMinorVersion >= 700 && programMajorMinorVersion < 710) ||
                    (programMajorMinorVersion >= 600 && programMajorMinorVersion < 610))
            ) {
                const parameters = jsTest.options().setParameters;
                const reshardingDefaults = {
                    "reshardingDelayBeforeRemainingOperationTimeQueryMillis": 0,
                };

                Object.entries(reshardingDefaults).forEach(([key, value]) => {
                    const keyIsNotParameter = parameters === undefined || parameters[key] === undefined;
                    const keyIsNotArgument = !argArrayContainsSetParameterValue(`${key}=`);

                    if (keyIsNotParameter && keyIsNotArgument) {
                        argArray.push("--setParameter", `${key}=${value}`);
                    }
                });
            }

            // New mongod-specific option in 4.9.x.
            if (programMajorMinorVersion >= 490) {
                const parameters = jsTest.options().setParameters;
                const reshardingDefaults = {
                    "reshardingMinimumOperationDurationMillis": "5000",
                    "reshardingCriticalSectionTimeoutMillis": 24 * 60 * 60 * 1000, // 24 hours
                };

                Object.entries(reshardingDefaults).forEach(([key, value]) => {
                    const keyIsNotParameter = parameters === undefined || parameters[key] === undefined;
                    const keyIsNotArgument = !argArrayContainsSetParameterValue(`${key}=`);

                    if (keyIsNotParameter && keyIsNotArgument) {
                        argArray.push("--setParameter", `${key}=${value}`);
                    }
                });
            }

            // New mongod-specific option in 4.5.x.
            if (programMajorMinorVersion >= 450) {
                // Allow the parameter to be overridden if set explicitly via TestData.
                const parameters = jsTest.options().setParameters;

                if (
                    (parameters === undefined ||
                        parameters["coordinateCommitReturnImmediatelyAfterPersistingDecision"] === undefined) &&
                    !argArrayContainsSetParameterValue("coordinateCommitReturnImmediatelyAfterPersistingDecision=")
                ) {
                    argArray.push(
                        ...["--setParameter", "coordinateCommitReturnImmediatelyAfterPersistingDecision=false"],
                    );
                }
            }

            // New mongod-specific option in 4.4.
            if (programMajorMinorVersion >= 440) {
                if (
                    jsTest.options().setParameters &&
                    jsTest.options().setParameters["enableIndexBuildCommitQuorum"] !== undefined
                ) {
                    if (!argArrayContainsSetParameterValue("enableIndexBuildCommitQuorum=")) {
                        argArray.push(
                            ...[
                                "--setParameter",
                                "enableIndexBuildCommitQuorum=" +
                                    jsTest.options().setParameters["enableIndexBuildCommitQuorum"],
                            ],
                        );
                    }
                }
            }

            // New mongod-specific option in 4.5.
            if (programMajorMinorVersion >= 450) {
                // Allow the parameter to be overridden if set explicitly via TestData.
                if (
                    (jsTest.options().setParameters === undefined ||
                        jsTest.options().setParameters["oplogApplicationEnforcesSteadyStateConstraints"] ===
                            undefined) &&
                    !argArrayContainsSetParameterValue("oplogApplicationEnforcesSteadyStateConstraints=")
                ) {
                    argArray.push(...["--setParameter", "oplogApplicationEnforcesSteadyStateConstraints=true"]);
                }
            }

            // New mongod-specific options in 4.0.x
            if (programMajorMinorVersion >= 400) {
                if (jsTest.options().transactionLifetimeLimitSeconds !== undefined) {
                    if (!argArrayContainsSetParameterValue("transactionLifetimeLimitSeconds=")) {
                        argArray.push(
                            ...[
                                "--setParameter",
                                "transactionLifetimeLimitSeconds=" + jsTest.options().transactionLifetimeLimitSeconds,
                            ],
                        );
                    }
                }
            }

            // Reduce the default value for `orphanCleanupDelaySecs` to 1 sec to speed up jstests.
            if (programMajorMinorVersion >= 360) {
                if (!argArrayContainsSetParameterValue("orphanCleanupDelaySecs=")) {
                    argArray.push(...["--setParameter", "orphanCleanupDelaySecs=1"]);
                }
            }

            // Increase the default value for `receiveChunkWaitForRangeDeleterTimeoutMS` to 90
            // seconds to prevent failures due to occasional slow range deletions
            if (programMajorMinorVersion >= 440) {
                if (!argArrayContainsSetParameterValue("receiveChunkWaitForRangeDeleterTimeoutMS=")) {
                    argArray.push(...["--setParameter", "receiveChunkWaitForRangeDeleterTimeoutMS=90000"]);
                }
            }

            // Since options may not be backward compatible, mongod options are not
            // set on older versions, e.g., mongod-3.0.
            if (baseProgramName === "mongod" && programMajorMinorVersion == lastestMajorMinorVersion) {
                if (jsTest.options().storageEngine === "wiredTiger" || !jsTest.options().storageEngine) {
                    if (jsTest.options().storageEngineCacheSizeGB && !argArrayContains("--wiredTigerCacheSizeGB")) {
                        argArray.push(...["--wiredTigerCacheSizeGB", jsTest.options().storageEngineCacheSizeGB]);
                    }
                    if (jsTest.options().storageEngineCacheSizePct && !argArrayContains("--wiredTigerCacheSizePct")) {
                        argArray.push(...["--wiredTigerCacheSizePct", jsTest.options().storageEngineCacheSizePct]);
                    }
                    if (
                        jsTest.options().wiredTigerEngineConfigString &&
                        !argArrayContains("--wiredTigerEngineConfigString")
                    ) {
                        argArray.push(
                            ...["--wiredTigerEngineConfigString", jsTest.options().wiredTigerEngineConfigString],
                        );
                    }
                    if (
                        jsTest.options().wiredTigerCollectionConfigString &&
                        !argArrayContains("--wiredTigerCollectionConfigString")
                    ) {
                        argArray.push(
                            ...[
                                "--wiredTigerCollectionConfigString",
                                jsTest.options().wiredTigerCollectionConfigString,
                            ],
                        );
                    }
                    if (
                        jsTest.options().wiredTigerIndexConfigString &&
                        !argArrayContains("--wiredTigerIndexConfigString")
                    ) {
                        argArray.push(
                            ...["--wiredTigerIndexConfigString", jsTest.options().wiredTigerIndexConfigString],
                        );
                    }
                } else if (jsTest.options().storageEngine === "inMemory") {
                    if (jsTest.options().storageEngineCacheSizeGB && !argArrayContains("--inMemorySizeGB")) {
                        argArray.push(...["--inMemorySizeGB", jsTest.options().storageEngineCacheSizeGB]);
                    }
                }
                // apply setParameters for mongod. The 'setParameters' field should be given as
                // a plain JavaScript object, where each key is a parameter name and the value
                // is the value to set for that parameter.
                if (jsTest.options().setParameters) {
                    let params = jsTest.options().setParameters;
                    for (let paramName of Object.keys(params)) {
                        if (argArrayContainsSetParameterValue(paramName + "=")) {
                            continue;
                        }

                        const paramVal = ((param) => {
                            if (typeof param === "object") {
                                return JSON.stringify(param);
                            }

                            return param;
                        })(params[paramName]);
                        const setParamStr = paramName + "=" + paramVal;
                        argArray.push(...["--setParameter", setParamStr]);
                    }
                }
            }
        }

        // Increase the default config server command timeout to 5 minutes to avoid spurious
        // failures on slow machines. New option in 7.3. Applies to both mongos and mongod and
        // should not overwrite values passed via TestData, so we check after merging TestData
        // setParameters into the arg array.
        if (programMajorMinorVersion >= 730) {
            if (!argArrayContainsSetParameterValue("defaultConfigCommandTimeoutMS=")) {
                argArray.push(...["--setParameter", "defaultConfigCommandTimeoutMS=300000"]);
            }
        }
    }

    return argArray;
}

/**
 * Continuously tries to establish a connection to the server on the specified port.
 *
 * If a connection cannot be established within a time limit, or if the process terminated
 * with a non-zero exit code, an exception will be thrown. If the process for the given
 * 'pid' is found to have gracefully terminated, this function will terminate and return
 * null.
 *
 * @param {int} [pid] the process id of the node to connect to.
 * @param {int} [port] the port of the node to connect to.
 * @returns a new Mongo connection object, or null if the process gracefully terminated.
 */
MongoRunner.awaitConnection = function (
    {pid, port, waitTimeoutMS} = {
        waitTimeoutMS: 600 * 1000,
    },
) {
    let conn = null;
    assert.soon(
        function () {
            try {
                conn = new Mongo("127.0.0.1:" + port);
                conn.pid = pid;
                return true;
            } catch (e) {
                let res = checkProgram(pid);
                if (!res.alive) {
                    print(
                        "mongo program was not running at " + port + ", process ended with exit code: " + res.exitCode,
                    );
                    serverExitCodeMap[port] = res.exitCode;
                    if (res.exitCode !== MongoRunner.EXIT_CLEAN) {
                        throw new MongoRunner.StopError(res.exitCode);
                    }
                    return true;
                }
            }
            return false;
        },
        "unable to connect to mongo program on port " + port,
        waitTimeoutMS,
    );
    return conn;
};

/**
 * Start a mongo process with a particular argument array.
 * If we aren't waiting for connect, return {pid: <pid>}.
 * If we are waiting for connect:
 *     returns connection to process on success;
 *     otherwise returns null if we fail to connect.
 */
MongoRunner._startWithArgs = function (argArray, env, waitForConnect, waitForConnectTimeoutMS) {
    // TODO: Make there only be one codepath for starting mongo processes

    // The config fuzzer does not apply all fuzzed configurations to mongo binaries executed via
    // MongoRunner (See SERVER-92148 and SERVER-92894). To avoid a situation where programmers
    // develop tests believing otherwise, we make doing so an explicit error.
    assert(
        !jsTest.options().fuzzMongodConfigs,
        "runMongo*() cannot be used with a fuzzed configuration, use standalone.py instead.",
    );

    argArray = appendSetParameterArgs(argArray);
    let port = MongoRunner.parsePort.apply(null, argArray);
    let pid = -1;

    if (jsTest.options().mozJSGCZeal) {
        if (env === undefined) {
            env = {};
        }
        env["JS_GC_ZEAL"] = jsTest.options().mozJSGCZeal;
    }

    if (env === undefined) {
        pid = _startMongoProgram(...argArray);
    } else {
        pid = _startMongoProgram({args: argArray, env});
    }

    delete serverExitCodeMap[port];
    if (!waitForConnect) {
        print("Skip waiting to connect to node with pid=" + pid + ", port=" + port);
        return {
            pid,
            port,
        };
    }

    return MongoRunner.awaitConnection({pid, port, waitTimeoutMS: waitForConnectTimeoutMS});
};

/**
 * Start mongod or mongos and return a Mongo() object connected to there.
 * This function's first argument is "mongod" or "mongos" program name, \
 * and subsequent arguments to this function are passed as
 * command line arguments to the program.
 *
 * @deprecated
 */
function startMongoProgram(...args) {
    let port = MongoRunner.parsePort(...args);

    // Enable test commands.
    // TODO: Make this work better with multi-version testing so that we can support
    // enabling this on 2.4 when testing 2.6
    args = appendSetParameterArgs(args);
    let pid = _startMongoProgram(...args);

    let m;
    assert.soon(
        function () {
            try {
                m = new Mongo("127.0.0.1:" + port);
                m.pid = pid;
                return true;
            } catch (e) {
                let res = checkProgram(pid);
                if (!res.alive) {
                    print(
                        "Could not start mongo program at " + port + ", process ended with exit code: " + res.exitCode,
                    );
                    // Break out
                    m = null;
                    return true;
                }
            }
            return false;
        },
        "unable to connect to mongo program on port " + port,
        600 * 1000,
    );

    return m;
}

function _getMongoProgramArguments(args) {
    args = Array.from(args);
    appendSetParameterArgs(args);
    let progName = args[0];

    const separator = _isWindows() ? "\\" : "/";
    progName = progName.split(separator).pop();
    const [baseProgramName] = progName.split("-");
    let newArgs = [];

    // Non-shell binaries (which are in fact instantiated via `runMongoProgram`) may not support
    // these command line flags.
    if (jsTestOptions().auth && baseProgramName != "mongod") {
        newArgs.push(
            "-u",
            jsTestOptions().authUser,
            "-p",
            jsTestOptions().authPassword,
            "--authenticationDatabase=admin",
        );
    }
    if (baseProgramName == "mongo") {
        if (jsTestOptions().shellTlsEnabled) {
            newArgs.push("--tls");
            newArgs.push("--tlsAllowInvalidHostnames");
        }
        if (jsTestOptions().shellTlsCertificateKeyFile && !args.includes("--tlsCertificateKeyFile")) {
            newArgs.push("--tlsCertificateKeyFile", jsTestOptions().shellTlsCertificateKeyFile);
        }
        if (jsTestOptions().shellGRPC && !args.includes("--gRPC")) {
            newArgs.push("--gRPC");
        }
    } else {
        if (jsTestOptions().tlsMode && !args.includes("--tlsMode")) {
            newArgs.push("--tlsMode", jsTestOptions().tlsMode);
            newArgs.push("--tlsAllowInvalidHostnames");
            if (!jsTestOptions().shellTlsCertificateKeyFile) {
                newArgs.push("--tlsAllowConnectionsWithoutCertificates");
            }
        }

        if (!args.includes("--tlsCertificateKeyFile")) {
            if (baseProgramName == "mongod" && jsTestOptions().mongodTlsCertificateKeyFile) {
                newArgs.push("--tlsCertificateKeyFile", jsTestOptions().mongodTlsCertificateKeyFile);
            } else if (baseProgramName == "mongos" && jsTestOptions().mongosTlsCertificateKeyFile) {
                newArgs.push("--tlsCertificateKeyFile", jsTestOptions().mongosTlsCertificateKeyFile);
            }
        }
    }

    if (jsTestOptions().tlsCAFile && !args.includes("--tlsCAFile")) {
        newArgs.push("--tlsCAFile", jsTestOptions().tlsCAFile);
    }

    args = args.slice(1);
    args.unshift(progName, ...newArgs);
    return args;
}

function runMongoProgram(...args) {
    return _runMongoProgram(..._getMongoProgramArguments(args));
}

/**
 *  Start a mongo program instance.  This function's first argument is the
 * program name, and subsequent arguments to this function are passed as
 * command line arguments to the program.  Returns pid of the spawned program.
 */
function startMongoProgramNoConnect(...args) {
    return _startMongoProgram(..._getMongoProgramArguments(args));
}

export {MongoRunner, runMongoProgram, startMongoProgram, startMongoProgramNoConnect};
