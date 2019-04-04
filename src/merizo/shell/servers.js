var MerizoRunner, _startMerizod, startMerizoProgram, runMerizoProgram, startMerizoProgramNoConnect,
    myPort;

(function() {
    "use strict";

    var shellVersion = version;

    // Record the exit codes of merizod and merizos processes that crashed during startup keyed by
    // port. This map is cleared when MerizoRunner._startWithArgs and MerizoRunner.stopMerizod/s are
    // called.
    var serverExitCodeMap = {};

    var _parsePath = function() {
        var dbpath = "";
        for (var i = 0; i < arguments.length; ++i)
            if (arguments[i] == "--dbpath")
                dbpath = arguments[i + 1];

        if (dbpath == "")
            throw Error("No dbpath specified");

        return dbpath;
    };

    var _parsePort = function() {
        var port = "";
        for (var i = 0; i < arguments.length; ++i)
            if (arguments[i] == "--port")
                port = arguments[i + 1];

        if (port == "")
            throw Error("No port specified");
        return port;
    };

    var createMerizoArgs = function(binaryName, args) {
        if (!Array.isArray(args)) {
            throw new Error("The second argument to createMerizoArgs must be an array");
        }

        var fullArgs = [binaryName];

        if (args.length == 1 && isObject(args[0])) {
            var o = args[0];
            for (var k in o) {
                if (o.hasOwnProperty(k)) {
                    if (k == "v" && isNumber(o[k])) {
                        var n = o[k];
                        if (n > 0) {
                            if (n > 10)
                                n = 10;
                            var temp = "-";
                            while (n-- > 0)
                                temp += "v";
                            fullArgs.push(temp);
                        }
                    } else {
                        fullArgs.push("--" + k);
                        if (o[k] != "")
                            fullArgs.push("" + o[k]);
                    }
                }
            }
        } else {
            for (var i = 0; i < args.length; i++)
                fullArgs.push(args[i]);
        }

        return fullArgs;
    };

    MerizoRunner = function() {};

    MerizoRunner.dataDir = "/data/db";
    MerizoRunner.dataPath = "/data/db/";

    MerizoRunner.merizodPath = "merizod";
    MerizoRunner.merizosPath = "merizos";
    MerizoRunner.merizoShellPath = "merizo";

    MerizoRunner.VersionSub = function(pattern, version) {
        this.pattern = pattern;
        this.version = version;
    };

    /**
     * Returns an array of version elements from a version string.
     *
     * "3.3.4-fade3783" -> ["3", "3", "4-fade3783" ]
     * "3.2" -> [ "3", "2" ]
     * 3 -> exception: versions must have at least two components.
     */
    var convertVersionStringToArray = function(versionString) {
        assert("" !== versionString, "Version strings must not be empty");
        var versionArray = versionString.split('.');

        assert.gt(versionArray.length,
                  1,
                  "MerizoDB versions must have at least two components to compare, but \"" +
                      versionString + "\" has " + versionArray.length);
        return versionArray;
    };

    /**
     * Returns the major version string from a version string.
     *
     * 3.3.4-fade3783 -> 3.3
     * 3.2 -> 3.2
     * 3 -> exception: versions must have at least two components.
     */
    var extractMajorVersionFromVersionString = function(versionString) {
        return convertVersionStringToArray(versionString).slice(0, 2).join('.');
    };

    // These patterns allow substituting the binary versions used for each version string to support
    // the
    // dev/stable MerizoDB release cycle.
    //
    // If you add a new version substitution to this list, you should add it to the lists of
    // versions being checked in 'verify_versions_test.js' to verify it is susbstituted correctly.
    MerizoRunner.binVersionSubs = [
        new MerizoRunner.VersionSub("latest", shellVersion()),
        new MerizoRunner.VersionSub(extractMajorVersionFromVersionString(shellVersion()),
                                   shellVersion()),
        // To-be-updated when we branch for the next release.
        new MerizoRunner.VersionSub("last-stable", "4.0")
    ];

    MerizoRunner.getBinVersionFor = function(version) {
        if (version instanceof MerizoRunner.versionIterator.iterator) {
            version = version.current();
        }

        if (version == null)
            version = "";
        version = version.trim();
        if (version === "")
            version = "latest";

        // See if this version is affected by version substitutions
        for (var i = 0; i < MerizoRunner.binVersionSubs.length; i++) {
            var sub = MerizoRunner.binVersionSubs[i];
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
    MerizoRunner.areBinVersionsTheSame = function(versionA, versionB) {

        // Check for invalid version strings first.
        convertVersionStringToArray(MerizoRunner.getBinVersionFor(versionA));
        convertVersionStringToArray(MerizoRunner.getBinVersionFor(versionB));

        try {
            return (0 === MerizoRunner.compareBinVersions(versionA, versionB));
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
    MerizoRunner.compareBinVersions = function(versionA, versionB) {

        let stringA = versionA;
        let stringB = versionB;

        versionA = convertVersionStringToArray(MerizoRunner.getBinVersionFor(versionA));
        versionB = convertVersionStringToArray(MerizoRunner.getBinVersionFor(versionB));

        // Treat the githash as a separate element, if it's present.
        versionA.push(...versionA.pop().split("-"));
        versionB.push(...versionB.pop().split("-"));

        var elementsToCompare = Math.min(versionA.length, versionB.length);

        for (var i = 0; i < elementsToCompare; ++i) {
            var elementA = versionA[i];
            var elementB = versionB[i];

            if (elementA === elementB) {
                continue;
            }

            var numA = parseInt(elementA);
            var numB = parseInt(elementB);

            assert(!isNaN(numA) && !isNaN(numB), "Cannot compare non-equal non-numeric versions.");

            if (numA > numB) {
                return 1;
            } else if (numA < numB) {
                return -1;
            }

            assert(false, `Unreachable case. Provided versions: {${stringA}, ${stringB}}`);
        }

        return 0;
    };

    MerizoRunner.logicalOptions = {
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
        noJournal: true,
        binVersion: true,
        waitForConnect: true,
        bridgeOptions: true
    };

    MerizoRunner.toRealPath = function(path, pathOpts) {

        // Replace all $pathOptions with actual values
        pathOpts = pathOpts || {};
        path = path.replace(/\$dataPath/g, MerizoRunner.dataPath);
        path = path.replace(/\$dataDir/g, MerizoRunner.dataDir);
        for (var key in pathOpts) {
            path = path.replace(RegExp("\\$" + RegExp.escape(key), "g"), pathOpts[key]);
        }

        // Relative path
        // Detect Unix and Windows absolute paths
        // as well as Windows drive letters
        // Also captures Windows UNC paths

        if (!path.match(/^(\/|\\|[A-Za-z]:)/)) {
            if (path != "" && !path.endsWith("/"))
                path += "/";

            path = MerizoRunner.dataPath + path;
        }

        return path;

    };

    MerizoRunner.toRealDir = function(path, pathOpts) {

        path = MerizoRunner.toRealPath(path, pathOpts);

        if (path.endsWith("/"))
            path = path.substring(0, path.length - 1);

        return path;
    };

    MerizoRunner.toRealFile = MerizoRunner.toRealDir;

    /**
     * Returns an iterator object which yields successive versions on calls to advance(), starting
     * from a random initial position, from an array of versions.
     *
     * If passed a single version string or an already-existing version iterator, just returns the
     * object itself, since it will yield correctly on calls to advance().
     *
     * @param {Array.<String>}|{String}|{versionIterator}
     */
    MerizoRunner.versionIterator = function(arr, isRandom) {

        // If this isn't an array of versions, or is already an iterator, just use it
        if (typeof arr == "string")
            return arr;
        if (arr.isVersionIterator)
            return arr;

        if (isRandom == undefined)
            isRandom = false;

        // Starting pos
        var i = isRandom ? parseInt(Random.rand() * arr.length) : 0;

        return new MerizoRunner.versionIterator.iterator(i, arr);
    };

    MerizoRunner.versionIterator.iterator = function(i, arr) {
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
     * are defined in MerizoRunner.logicalOptions, of which they will be ignored.
     *
     * @param {string} binaryName
     * @param {Object} args
     *
     * @return {Array.<String>} an array of parameter strings that can be passed
     *   to the binary.
     */
    MerizoRunner.arrOptions = function(binaryName, args) {

        var fullArgs = [""];

        // isObject returns true even if "args" is an array, so the else branch of this statement is
        // dead code.  See SERVER-14220.
        if (isObject(args) || (args.length == 1 && isObject(args[0]))) {
            var o = isObject(args) ? args : args[0];

            // If we've specified a particular binary version, use that
            if (o.binVersion && o.binVersion != "" && o.binVersion != shellVersion()) {
                binaryName += "-" + o.binVersion;
            }

            // Manage legacy options
            var isValidOptionForBinary = function(option, value) {

                if (!o.binVersion)
                    return true;

                return true;
            };

            var addOptionsToFullArgs = function(k, v) {
                if (v === undefined || v === null)
                    return;

                fullArgs.push("--" + k);

                if (v != "") {
                    fullArgs.push("" + v);
                }
            };

            for (var k in o) {
                // Make sure our logical option should be added to the array of options
                if (!o.hasOwnProperty(k) || k in MerizoRunner.logicalOptions ||
                    !isValidOptionForBinary(k, o[k]))
                    continue;

                if ((k == "v" || k == "verbose") && isNumber(o[k])) {
                    var n = o[k];
                    if (n > 0) {
                        if (n > 10)
                            n = 10;
                        var temp = "-";
                        while (n-- > 0)
                            temp += "v";
                        fullArgs.push(temp);
                    }
                } else if (k === "setParameter" && isObject(o[k])) {
                    // If the value associated with the setParameter option is an object, we want
                    // to add all key-value pairs in that object as separate --setParameters.
                    Object.keys(o[k]).forEach(function(paramKey) {
                        addOptionsToFullArgs(k, "" + paramKey + "=" + o[k][paramKey]);
                    });
                } else {
                    addOptionsToFullArgs(k, o[k]);
                }
            }
        } else {
            for (var i = 0; i < args.length; i++)
                fullArgs.push(args[i]);
        }

        fullArgs[0] = binaryName;
        return fullArgs;
    };

    MerizoRunner.arrToOpts = function(arr) {

        var opts = {};
        for (var i = 1; i < arr.length; i++) {
            if (arr[i].startsWith("-")) {
                var opt = arr[i].replace(/^-/, "").replace(/^-/, "");

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

    MerizoRunner.savedOptions = {};

    MerizoRunner.merizoOptions = function(opts) {
        // Don't remember waitForConnect
        var waitForConnect = opts.waitForConnect;
        delete opts.waitForConnect;

        // If we're a merizo object
        if (opts.getDB) {
            opts = {restart: opts.runId};
        }

        // Initialize and create a copy of the opts
        opts = Object.merge(opts || {}, {});

        if (!opts.restart)
            opts.restart = false;

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
        if (isObject(opts.runId) && opts.runId.runId)
            opts.runId = opts.runId.runId;

        if (opts.restart && opts.remember) {
            opts = Object.merge(MerizoRunner.savedOptions[opts.runId], opts);
        }

        // Create a new runId
        opts.runId = opts.runId || ObjectId();

        if (opts.forgetPort) {
            delete opts.port;
        }

        // Normalize and get the binary version to use
        if (opts.hasOwnProperty('binVersion')) {
            if (opts.binVersion instanceof MerizoRunner.versionIterator.iterator) {
                // Advance the version iterator so that subsequent calls to
                // MerizoRunner.merizoOptions() use the next version in the list.
                const iterator = opts.binVersion;
                opts.binVersion = iterator.current();
                iterator.advance();
            }
            opts.binVersion = MerizoRunner.getBinVersionFor(opts.binVersion);
        }

        // Default for waitForConnect is true
        opts.waitForConnect =
            (waitForConnect == undefined || waitForConnect == null) ? true : waitForConnect;

        opts.port = opts.port || allocatePort();

        opts.pathOpts =
            Object.merge(opts.pathOpts || {}, {port: "" + opts.port, runId: "" + opts.runId});

        var shouldRemember =
            (!opts.restart && !opts.noRemember) || (opts.restart && opts.appendOptions);
        if (shouldRemember) {
            MerizoRunner.savedOptions[opts.runId] = Object.merge(opts, {});
        }

        if (jsTestOptions().networkMessageCompressors) {
            opts.networkMessageCompressors = jsTestOptions().networkMessageCompressors;
        }

        if (!opts.hasOwnProperty('bind_ip')) {
            opts.bind_ip = "0.0.0.0";
        }

        return opts;
    };

    // Returns an array of integers representing the version provided.
    // Ex: "3.3.12" => [3, 3, 12]
    var _convertVersionToIntegerArray = function(version) {
        var versionParts =
            convertVersionStringToArray(version).slice(0, 3).map(part => parseInt(part, 10));
        if (versionParts.length === 2) {
            versionParts.push(Infinity);
        }
        return versionParts;
    };

    // Returns if version2 is equal to, or came after, version 1.
    var _isMerizodVersionEqualOrAfter = function(version1, version2) {
        if (version2 === "latest") {
            return true;
        }

        var versionParts1 = _convertVersionToIntegerArray(version1);
        var versionParts2 = _convertVersionToIntegerArray(version2);
        if (versionParts2[0] > versionParts1[0] ||
            (versionParts2[0] === versionParts1[0] && versionParts2[1] > versionParts1[1]) ||
            (versionParts2[0] === versionParts1[0] && versionParts2[1] === versionParts1[1] &&
             versionParts2[2] >= versionParts1[2])) {
            return true;
        }

        return false;
    };

    // Removes a setParameter parameter from merizods running a version that won't recognize them.
    var _removeSetParameterIfBeforeVersion = function(opts, parameterName, requiredVersion) {
        var versionCompatible = (opts.binVersion === "" || opts.binVersion === undefined ||
                                 _isMerizodVersionEqualOrAfter(requiredVersion, opts.binVersion));
        if (!versionCompatible && opts.setParameter &&
            opts.setParameter[parameterName] != undefined) {
            print("Removing '" + parameterName + "' setParameter with value " +
                  opts.setParameter[parameterName] +
                  " because it isn't compatibile with merizod running version " + opts.binVersion);
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
     *     noJournal {boolean}
     *     keyFile
     *     replSet
     *     oplogSize
     *   }
     */
    MerizoRunner.merizodOptions = function(opts) {

        opts = MerizoRunner.merizoOptions(opts);

        opts.dbpath = MerizoRunner.toRealDir(opts.dbpath || "$dataDir/merizod-$port", opts.pathOpts);

        opts.pathOpts = Object.merge(opts.pathOpts, {dbpath: opts.dbpath});

        _removeSetParameterIfBeforeVersion(opts, "writePeriodicNoops", "3.3.12");
        _removeSetParameterIfBeforeVersion(opts, "numInitialSyncAttempts", "3.3.12");
        _removeSetParameterIfBeforeVersion(opts, "numInitialSyncConnectAttempts", "3.3.12");
        _removeSetParameterIfBeforeVersion(opts, "migrationLockAcquisitionMaxWaitMS", "4.1.7");

        if (!opts.logFile && opts.useLogFiles) {
            opts.logFile = opts.dbpath + "/merizod.log";
        } else if (opts.logFile) {
            opts.logFile = MerizoRunner.toRealFile(opts.logFile, opts.pathOpts);
        }

        if (opts.logFile !== undefined) {
            opts.logpath = opts.logFile;
        }

        if ((jsTestOptions().noJournal || opts.noJournal) && !('journal' in opts) &&
            !('configsvr' in opts)) {
            opts.nojournal = "";
        }

        if (jsTestOptions().keyFile && !opts.keyFile) {
            opts.keyFile = jsTestOptions().keyFile;
        }

        if (opts.hasOwnProperty("enableEncryption")) {
            // opts.enableEncryption, if set, must be an empty string
            if (opts.enableEncryption !== "") {
                throw new Error("The enableEncryption option must be an empty string if it is " +
                                "specified");
            }
        } else if (jsTestOptions().enableEncryption !== undefined) {
            if (jsTestOptions().enableEncryption !== "") {
                throw new Error("The enableEncryption option must be an empty string if it is " +
                                "specified");
            }
            opts.enableEncryption = "";
        }

        if (opts.hasOwnProperty("encryptionKeyFile")) {
            // opts.encryptionKeyFile, if set, must be a string
            if (typeof opts.encryptionKeyFile !== "string") {
                throw new Error("The encryptionKeyFile option must be a string if it is specified");
            }
        } else if (jsTestOptions().encryptionKeyFile !== undefined) {
            if (typeof(jsTestOptions().encryptionKeyFile) !== "string") {
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
            if (typeof(jsTestOptions().auditDestination) !== "string") {
                throw new Error("The auditDestination option must be a string if it is specified");
            }
            opts.auditDestination = jsTestOptions().auditDestination;
        }

        if (opts.noReplSet)
            opts.replSet = null;
        if (opts.arbiter)
            opts.oplogSize = 1;

        return opts;
    };

    MerizoRunner.merizosOptions = function(opts) {
        opts = MerizoRunner.merizoOptions(opts);

        // Normalize configdb option to be host string if currently a host
        if (opts.configdb && opts.configdb.getDB) {
            opts.configdb = opts.configdb.host;
        }

        opts.pathOpts =
            Object.merge(opts.pathOpts, {configdb: opts.configdb.replace(/:|\/|,/g, "-")});

        if (!opts.logFile && opts.useLogFiles) {
            opts.logFile =
                MerizoRunner.toRealFile("$dataDir/merizos-$configdb-$port.log", opts.pathOpts);
        } else if (opts.logFile) {
            opts.logFile = MerizoRunner.toRealFile(opts.logFile, opts.pathOpts);
        }

        if (opts.logFile !== undefined) {
            opts.logpath = opts.logFile;
        }

        var testOptions = jsTestOptions();
        if (testOptions.keyFile && !opts.keyFile) {
            opts.keyFile = testOptions.keyFile;
        }

        if (opts.hasOwnProperty("auditDestination")) {
            // opts.auditDestination, if set, must be a string
            if (typeof opts.auditDestination !== "string") {
                throw new Error("The auditDestination option must be a string if it is specified");
            }
        } else if (testOptions.auditDestination !== undefined) {
            if (typeof(testOptions.auditDestination) !== "string") {
                throw new Error("The auditDestination option must be a string if it is specified");
            }
            opts.auditDestination = testOptions.auditDestination;
        }

        if (!opts.hasOwnProperty('binVersion') && testOptions.merizosBinVersion) {
            opts.binVersion = MerizoRunner.getBinVersionFor(testOptions.merizosBinVersion);
        }

        // If the merizos is being restarted with a newer version, make sure we remove any options
        // that no longer exist in the newer version.
        if (opts.restart && MerizoRunner.areBinVersionsTheSame('latest', opts.binVersion)) {
            delete opts.noAutoSplit;
        }

        return opts;
    };

    /**
     * Starts a merizod instance.
     *
     * @param {Object} opts
     *
     *   {
     *     useHostName {boolean}: Uses hostname of machine if true.
     *     forceLock {boolean}: Deletes the lock file if set to true.
     *     dbpath {string}: location of db files.
     *     cleanData {boolean}: Removes all files in dbpath if true.
     *     startClean {boolean}: same as cleanData.
     *     noCleanData {boolean}: Do not clean files (cleanData takes priority).
     *     binVersion {string}: version for binary (also see MerizoRunner.binVersionSubs).
     *
     *     @see MerizoRunner.merizodOptions for other options
     *   }
     *
     * @return {Merizo} connection object to the started merizod instance.
     *
     * @see MerizoRunner.arrOptions
     */
    MerizoRunner.runMerizod = function(opts) {

        opts = opts || {};
        var env = undefined;
        var useHostName = true;
        var runId = null;
        var waitForConnect = true;
        var fullOptions = opts;

        if (isObject(opts)) {
            opts = MerizoRunner.merizodOptions(opts);
            fullOptions = opts;

            if (opts.useHostName != undefined) {
                useHostName = opts.useHostName;
            } else if (opts.useHostname != undefined) {
                useHostName = opts.useHostname;
            } else {
                useHostName = true;  // Default to true
            }
            env = opts.env;
            runId = opts.runId;
            waitForConnect = opts.waitForConnect;

            if (opts.forceLock)
                removeFile(opts.dbpath + "/merizod.lock");
            if ((opts.cleanData || opts.startClean) || (!opts.restart && !opts.noCleanData)) {
                print("Resetting db path '" + opts.dbpath + "'");
                resetDbpath(opts.dbpath);
            }

            var merizodProgram = MerizoRunner.merizodPath;
            opts = MerizoRunner.arrOptions(merizodProgram, opts);
        }

        var merizod = MerizoRunner._startWithArgs(opts, env, waitForConnect);
        if (!merizod) {
            return null;
        }

        merizod.commandLine = MerizoRunner.arrToOpts(opts);
        merizod.name = (useHostName ? getHostName() : "localhost") + ":" + merizod.commandLine.port;
        merizod.host = merizod.name;
        merizod.port = parseInt(merizod.commandLine.port);
        merizod.runId = runId || ObjectId();
        merizod.dbpath = fullOptions.dbpath;
        merizod.savedOptions = MerizoRunner.savedOptions[merizod.runId];
        merizod.fullOptions = fullOptions;

        return merizod;
    };

    MerizoRunner.runMerizos = function(opts) {
        opts = opts || {};

        var env = undefined;
        var useHostName = false;
        var runId = null;
        var waitForConnect = true;
        var fullOptions = opts;

        if (isObject(opts)) {
            opts = MerizoRunner.merizosOptions(opts);
            fullOptions = opts;

            useHostName = opts.useHostName || opts.useHostname;
            runId = opts.runId;
            waitForConnect = opts.waitForConnect;
            env = opts.env;
            var merizosProgram = MerizoRunner.merizosPath;
            opts = MerizoRunner.arrOptions(merizosProgram, opts);
        }

        var merizos = MerizoRunner._startWithArgs(opts, env, waitForConnect);
        if (!merizos) {
            return null;
        }

        merizos.commandLine = MerizoRunner.arrToOpts(opts);
        merizos.name = (useHostName ? getHostName() : "localhost") + ":" + merizos.commandLine.port;
        merizos.host = merizos.name;
        merizos.port = parseInt(merizos.commandLine.port);
        merizos.runId = runId || ObjectId();
        merizos.savedOptions = MerizoRunner.savedOptions[merizos.runId];
        merizos.fullOptions = fullOptions;

        return merizos;
    };

    MerizoRunner.StopError = function(returnCode) {
        this.name = "StopError";
        this.returnCode = returnCode;
        this.message = "MerizoDB process stopped with exit code: " + this.returnCode;
        this.stack = this.toString() + "\n" + (new Error()).stack;
    };

    MerizoRunner.StopError.prototype = Object.create(Error.prototype);
    MerizoRunner.StopError.prototype.constructor = MerizoRunner.StopError;

    // Constants for exit codes of MerizoDB processes
    MerizoRunner.EXIT_ABORT = -6;
    MerizoRunner.EXIT_CLEAN = 0;
    MerizoRunner.EXIT_BADOPTIONS = 2;
    MerizoRunner.EXIT_REPLICATION_ERROR = 3;
    MerizoRunner.EXIT_NEED_UPGRADE = 4;
    MerizoRunner.EXIT_SHARDING_ERROR = 5;
    // SIGKILL is translated to TerminateProcess() on Windows, which causes the program to
    // terminate with exit code 1.
    MerizoRunner.EXIT_SIGKILL = _isWindows() ? 1 : -9;
    MerizoRunner.EXIT_KILL = 12;
    MerizoRunner.EXIT_ABRUPT = 14;
    MerizoRunner.EXIT_NTSERVICE_ERROR = 20;
    MerizoRunner.EXIT_JAVA = 21;
    MerizoRunner.EXIT_OOM_MALLOC = 42;
    MerizoRunner.EXIT_OOM_REALLOC = 43;
    MerizoRunner.EXIT_FS = 45;
    MerizoRunner.EXIT_CLOCK_SKEW = 47;  // OpTime clock skew; deprecated
    MerizoRunner.EXIT_NET_ERROR = 48;
    MerizoRunner.EXIT_WINDOWS_SERVICE_STOP = 49;
    MerizoRunner.EXIT_POSSIBLE_CORRUPTION = 60;
    MerizoRunner.EXIT_UNCAUGHT = 100;  // top level exception that wasn't caught
    MerizoRunner.EXIT_TEST = 101;

    MerizoRunner.validateCollectionsCallback = function(port) {};

    /**
     * Kills a merizod process.
     *
     * @param {Merizo} conn the connection object to the process to kill
     * @param {number} signal The signal number to use for killing
     * @param {Object} opts Additional options. Format:
     *    {
     *      auth: {
     *        user {string}: admin user name
     *        pwd {string}: admin password
     *      },
     *      skipValidation: <bool>,
     *      allowedExitCode: <int>
     *    }
     *
     * Note: The auth option is required in a authenticated merizod running in Windows since
     *  it uses the shutdown command, which requires admin credentials.
     */
    MerizoRunner.stopMerizod = function(conn, signal, opts) {
        if (!conn.pid) {
            throw new Error("first arg must have a `pid` property; " +
                            "it is usually the object returned from MerizoRunner.runMerizod/s");
        }

        if (!conn.port) {
            throw new Error("first arg must have a `port` property; " +
                            "it is usually the object returned from MerizoRunner.runMerizod/s");
        }

        signal = parseInt(signal) || 15;
        opts = opts || {};

        var allowedExitCode = MerizoRunner.EXIT_CLEAN;

        if (opts.allowedExitCode) {
            allowedExitCode = opts.allowedExitCode;
        }

        var port = parseInt(conn.port);

        var pid = conn.pid;
        // If the return code is in the serverExitCodeMap, it means the server crashed on startup.
        // We just use the recorded return code instead of stopping the program.
        var returnCode;
        if (serverExitCodeMap.hasOwnProperty(port)) {
            returnCode = serverExitCodeMap[port];
            delete serverExitCodeMap[port];
        } else {
            // Invoke callback to validate collections and indexes before shutting down merizod.
            // We skip calling the callback function when the expected return code of
            // the merizod process is non-zero since it's likely the process has already exited.

            var skipValidation = false;
            if (opts.skipValidation) {
                skipValidation = true;
            }

            if (allowedExitCode === MerizoRunner.EXIT_CLEAN && !skipValidation) {
                MerizoRunner.validateCollectionsCallback(port);
            }

            returnCode = _stopMerizoProgram(port, signal, opts);
        }
        if (allowedExitCode !== returnCode) {
            throw new MerizoRunner.StopError(returnCode);
        } else if (returnCode !== MerizoRunner.EXIT_CLEAN) {
            print("MerizoDB process on port " + port + " intentionally exited with error code ",
                  returnCode);
        }

        return returnCode;
    };

    MerizoRunner.stopMerizos = MerizoRunner.stopMerizod;

    /**
     * Starts an instance of the specified merizo tool
     *
     * @param {String} binaryName - The name of the tool to run.
     * @param {Object} [opts={}] - Options of the form --flag or --key=value to pass to the tool.
     * @param {string} [opts.binVersion] - The version of the tool to run.
     *
     * @param {...string} positionalArgs - Positional arguments to pass to the tool after all
     * options have been specified. For example,
     * MerizoRunner.runMerizoTool("executable", {key: value}, arg1, arg2) would invoke
     * ./executable --key value arg1 arg2.
     *
     * @see MerizoRunner.arrOptions
     */
    MerizoRunner.runMerizoTool = function(binaryName, opts, ...positionalArgs) {

        var opts = opts || {};

        // Normalize and get the binary version to use
        if (opts.binVersion instanceof MerizoRunner.versionIterator.iterator) {
            // Advance the version iterator so that subsequent calls to MerizoRunner.runMerizoTool()
            // use the next version in the list.
            const iterator = opts.binVersion;
            opts.binVersion = iterator.current();
            iterator.advance();
        }
        opts.binVersion = MerizoRunner.getBinVersionFor(opts.binVersion);

        // Recent versions of the merizo tools support a --dialTimeout flag to set for how
        // long they retry connecting to a merizod or merizos process. We have them retry
        // connecting for up to 30 seconds to handle when the tests are run on a
        // resource-constrained host machine.
        //
        // The bsondump tool doesn't accept the --dialTimeout flag because it doesn't connect to a
        // merizod or merizos process.
        if (!opts.hasOwnProperty('dialTimeout') && binaryName !== 'bsondump' &&
            _toolVersionSupportsDialTimeout(opts.binVersion)) {
            opts['dialTimeout'] = '30';
        }

        // Convert 'opts' into an array of arguments.
        var argsArray = MerizoRunner.arrOptions(binaryName, opts);

        // Append any positional arguments that were specified.
        argsArray.push(...positionalArgs);

        return runMerizoProgram.apply(null, argsArray);

    };

    var _toolVersionSupportsDialTimeout = function(version) {
        if (version === "latest" || version === "") {
            return true;
        }
        var versionParts =
            convertVersionStringToArray(version).slice(0, 3).map(part => parseInt(part, 10));
        if (versionParts.length === 2) {
            versionParts.push(Infinity);
        }

        if (versionParts[0] > 3 || (versionParts[0] === 3 && versionParts[1] > 3)) {
            // The --dialTimeout command line option is supported by the tools
            // with a major version newer than 3.3.
            return true;
        }

        for (var supportedVersion of["3.3.4", "3.2.5", "3.0.12"]) {
            var supportedVersionParts = convertVersionStringToArray(supportedVersion)
                                            .slice(0, 3)
                                            .map(part => parseInt(part, 10));
            if (versionParts[0] === supportedVersionParts[0] &&
                versionParts[1] === supportedVersionParts[1] &&
                versionParts[2] >= supportedVersionParts[2]) {
                return true;
            }
        }
        return false;
    };

    // Given a test name figures out a directory for that test to use for dump files and makes sure
    // that directory exists and is empty.
    MerizoRunner.getAndPrepareDumpDirectory = function(testName) {
        var dir = MerizoRunner.dataPath + testName + "_external/";
        resetDbpath(dir);
        return dir;
    };

    // Start a merizod instance and return a 'Merizo' object connected to it.
    // This function's arguments are passed as command line arguments to merizod.
    // The specified 'dbpath' is cleared if it exists, created if not.
    // var conn = _startMerizodEmpty("--port", 30000, "--dbpath", "asdf");
    var _startMerizodEmpty = function() {
        var args = createMerizoArgs("merizod", Array.from(arguments));

        var dbpath = _parsePath.apply(null, args);
        resetDbpath(dbpath);

        return startMerizoProgram.apply(null, args);
    };

    _startMerizod = function() {
        print("startMerizod WARNING DELETES DATA DIRECTORY THIS IS FOR TESTING ONLY");
        return _startMerizodEmpty.apply(null, arguments);
    };

    /**
     * Returns a new argArray with any test-specific arguments added.
     */
    function appendSetParameterArgs(argArray) {
        function argArrayContains(key) {
            return (argArray
                        .filter((val) => {
                            return typeof val === "string" && val.indexOf(key) === 0;
                        })
                        .length > 0);
        }

        function argArrayContainsSetParameterValue(value) {
            assert(value.endsWith("="),
                   "Expected value argument to be of the form <parameterName>=");
            return argArray.some(function(el) {
                return typeof el === "string" && el.startsWith(value);
            });
        }

        // programName includes the version, e.g., merizod-3.2.
        // baseProgramName is the program name without any version information, e.g., merizod.
        let programName = argArray[0];

        let [baseProgramName, programVersion] = programName.split("-");
        let programMajorMinorVersion = 0;
        if (programVersion) {
            let [major, minor, point] = programVersion.split(".");
            programMajorMinorVersion = parseInt(major) * 100 + parseInt(minor);
        }

        if (baseProgramName === 'merizod' || baseProgramName === 'merizos') {
            if (jsTest.options().enableTestCommands) {
                argArray.push(...['--setParameter', "enableTestCommands=1"]);
            }
            if (jsTest.options().authMechanism && jsTest.options().authMechanism != "SCRAM-SHA-1") {
                if (!argArrayContainsSetParameterValue('authenticationMechanisms=')) {
                    argArray.push(
                        ...['--setParameter',
                            "authenticationMechanisms=" + jsTest.options().authMechanism]);
                }
            }
            if (jsTest.options().auth) {
                argArray.push(...['--setParameter', "enableLocalhostAuthBypass=false"]);
            }

            // New options in 3.5.x
            if (!programMajorMinorVersion || programMajorMinorVersion >= 305) {
                if (jsTest.options().serviceExecutor) {
                    if (!argArrayContains("--serviceExecutor")) {
                        argArray.push(...["--serviceExecutor", jsTest.options().serviceExecutor]);
                    }
                }

                if (jsTest.options().transportLayer) {
                    if (!argArrayContains("--transportLayer")) {
                        argArray.push(...["--transportLayer", jsTest.options().transportLayer]);
                    }
                }

                // Disable background cache refreshing to avoid races in tests
                argArray.push(...['--setParameter', "disableLogicalSessionCacheRefresh=true"]);
            }

            // Since options may not be backward compatible, merizos options are not
            // set on older versions, e.g., merizos-3.0.
            if (programName.endsWith('merizos')) {
                // apply setParameters for merizos
                if (jsTest.options().setParametersMerizos) {
                    let params = jsTest.options().setParametersMerizos;
                    for (let paramName of Object.keys(params)) {
                        // Only set the 'logComponentVerbosity' parameter if it has not already
                        // been specified in the given argument array. This means that any
                        // 'logComponentVerbosity' settings passed through via TestData will
                        // always be overridden by settings passed directly to MerizoRunner from
                        // within the shell.
                        if (paramName === "logComponentVerbosity" &&
                            argArrayContains("logComponentVerbosity")) {
                            continue;
                        }
                        const paramVal = params[paramName];
                        const setParamStr = paramName + "=" + JSON.stringify(paramVal);
                        argArray.push(...['--setParameter', setParamStr]);
                    }
                }
            } else if (baseProgramName === 'merizod') {
                if (jsTestOptions().roleGraphInvalidationIsFatal) {
                    argArray.push(...['--setParameter', "roleGraphInvalidationIsFatal=true"]);
                }

                // Set storageEngine for merizod. There was no storageEngine parameter before 3.0.
                if (jsTest.options().storageEngine &&
                    (!programVersion || programMajorMinorVersion >= 300)) {
                    if (!argArrayContains("--storageEngine")) {
                        argArray.push(...['--storageEngine', jsTest.options().storageEngine]);
                    }
                }

                // New merizod-specific options in 4.0.x
                if (!programMajorMinorVersion || programMajorMinorVersion >= 400) {
                    if (jsTest.options().transactionLifetimeLimitSeconds !== undefined) {
                        if (!argArrayContainsSetParameterValue(
                                "transactionLifetimeLimitSeconds=")) {
                            argArray.push(
                                ...["--setParameter",
                                    "transactionLifetimeLimitSeconds=" +
                                        jsTest.options().transactionLifetimeLimitSeconds]);
                        }
                    }
                }

                // TODO: Make this unconditional in 3.8.
                if (!programMajorMinorVersion || programMajorMinorVersion > 304) {
                    if (!argArrayContainsSetParameterValue('orphanCleanupDelaySecs=')) {
                        argArray.push(...['--setParameter', 'orphanCleanupDelaySecs=1']);
                    }
                }

                // Since options may not be backward compatible, merizod options are not
                // set on older versions, e.g., merizod-3.0.
                if (programName.endsWith('merizod')) {
                    if (jsTest.options().storageEngine === "wiredTiger" ||
                        !jsTest.options().storageEngine) {
                        if (jsTest.options().enableMajorityReadConcern !== undefined &&
                            !argArrayContains("--enableMajorityReadConcern")) {
                            argArray.push(
                                ...['--enableMajorityReadConcern',
                                    jsTest.options().enableMajorityReadConcern.toString()]);
                        }
                        if (jsTest.options().storageEngineCacheSizeGB &&
                            !argArrayContains('--wiredTigerCacheSizeGB')) {
                            argArray.push(...['--wiredTigerCacheSizeGB',
                                              jsTest.options().storageEngineCacheSizeGB]);
                        }
                        if (jsTest.options().wiredTigerEngineConfigString &&
                            !argArrayContains('--wiredTigerEngineConfigString')) {
                            argArray.push(...['--wiredTigerEngineConfigString',
                                              jsTest.options().wiredTigerEngineConfigString]);
                        }
                        if (jsTest.options().wiredTigerCollectionConfigString &&
                            !argArrayContains('--wiredTigerCollectionConfigString')) {
                            argArray.push(...['--wiredTigerCollectionConfigString',
                                              jsTest.options().wiredTigerCollectionConfigString]);
                        }
                        if (jsTest.options().wiredTigerIndexConfigString &&
                            !argArrayContains('--wiredTigerIndexConfigString')) {
                            argArray.push(...['--wiredTigerIndexConfigString',
                                              jsTest.options().wiredTigerIndexConfigString]);
                        }
                    } else if (jsTest.options().storageEngine === "rocksdb") {
                        if (jsTest.options().storageEngineCacheSizeGB) {
                            argArray.push(...['--rocksdbCacheSizeGB',
                                              jsTest.options().storageEngineCacheSizeGB]);
                        }
                    } else if (jsTest.options().storageEngine === "inMemory") {
                        if (jsTest.options().storageEngineCacheSizeGB &&
                            !argArrayContains("--inMemorySizeGB")) {
                            argArray.push(
                                ...["--inMemorySizeGB", jsTest.options().storageEngineCacheSizeGB]);
                        }
                    }
                    // apply setParameters for merizod. The 'setParameters' field should be given as
                    // a plain JavaScript object, where each key is a parameter name and the value
                    // is the value to set for that parameter.
                    if (jsTest.options().setParameters) {
                        let params = jsTest.options().setParameters;
                        for (let paramName of Object.keys(params)) {
                            // Only set the 'logComponentVerbosity' parameter if it has not already
                            // been specified in the given argument array. This means that any
                            // 'logComponentVerbosity' settings passed through via TestData will
                            // always be overridden by settings passed directly to MerizoRunner from
                            // within the shell.
                            if (paramName === "logComponentVerbosity" &&
                                argArrayContains("logComponentVerbosity")) {
                                continue;
                            }
                            const paramVal = params[paramName];
                            const setParamStr = paramName + "=" + JSON.stringify(paramVal);
                            argArray.push(...['--setParameter', setParamStr]);
                        }
                    }
                }
            }
        }

        return argArray;
    }

    /**
     * Start a merizo process with a particular argument array.
     * If we aren't waiting for connect, return {pid: <pid>}.
     * If we are waiting for connect:
     *     returns connection to process on success;
     *     otherwise returns null if we fail to connect.
     */
    MerizoRunner._startWithArgs = function(argArray, env, waitForConnect) {
        // TODO: Make there only be one codepath for starting merizo processes

        argArray = appendSetParameterArgs(argArray);
        var port = _parsePort.apply(null, argArray);
        var pid = -1;
        if (env === undefined) {
            pid = _startMerizoProgram.apply(null, argArray);
        } else {
            pid = _startMerizoProgram({args: argArray, env: env});
        }

        delete serverExitCodeMap[port];
        if (!waitForConnect) {
            return {
                pid: pid,
                port: port,
            };
        }

        var conn = null;
        assert.soon(function() {
            try {
                conn = new Merizo("127.0.0.1:" + port);
                conn.pid = pid;
                return true;
            } catch (e) {
                var res = checkProgram(pid);
                if (!res.alive) {
                    print("Could not start merizo program at " + port +
                          ", process ended with exit code: " + res.exitCode);
                    serverExitCodeMap[port] = res.exitCode;
                    return true;
                }
            }
            return false;
        }, "unable to connect to merizo program on port " + port, 600 * 1000);

        return conn;
    };

    /**
     * DEPRECATED
     *
     * Start merizod or merizos and return a Merizo() object connected to there.
     * This function's first argument is "merizod" or "merizos" program name, \
     * and subsequent arguments to this function are passed as
     * command line arguments to the program.
     */
    startMerizoProgram = function() {
        var port = _parsePort.apply(null, arguments);

        // Enable test commands.
        // TODO: Make this work better with multi-version testing so that we can support
        // enabling this on 2.4 when testing 2.6
        var args = Array.from(arguments);
        args = appendSetParameterArgs(args);
        var pid = _startMerizoProgram.apply(null, args);

        var m;
        assert.soon(function() {
            try {
                m = new Merizo("127.0.0.1:" + port);
                m.pid = pid;
                return true;
            } catch (e) {
                var res = checkProgram(pid);
                if (!res.alive) {
                    print("Could not start merizo program at " + port +
                          ", process ended with exit code: " + res.exitCode);
                    // Break out
                    m = null;
                    return true;
                }
            }
            return false;
        }, "unable to connect to merizo program on port " + port, 600 * 1000);

        return m;
    };

    runMerizoProgram = function() {
        var args = Array.from(arguments);
        args = appendSetParameterArgs(args);
        var progName = args[0];

        if (jsTestOptions().auth && progName != 'merizod') {
            args = args.slice(1);
            args.unshift(progName,
                         '-u',
                         jsTestOptions().authUser,
                         '-p',
                         jsTestOptions().authPassword,
                         '--authenticationDatabase=admin');
        }

        if (progName == 'merizo' && !_useWriteCommandsDefault()) {
            progName = args[0];
            args = args.slice(1);
            args.unshift(progName, '--useLegacyWriteOps');
        }

        return _runMerizoProgram.apply(null, args);
    };

    // Start a merizo program instance.  This function's first argument is the
    // program name, and subsequent arguments to this function are passed as
    // command line arguments to the program.  Returns pid of the spawned program.
    startMerizoProgramNoConnect = function() {
        var args = Array.from(arguments);
        args = appendSetParameterArgs(args);
        var progName = args[0];

        if (jsTestOptions().auth) {
            args = args.slice(1);
            args.unshift(progName,
                         '-u',
                         jsTestOptions().authUser,
                         '-p',
                         jsTestOptions().authPassword,
                         '--authenticationDatabase=admin');
        }

        if (progName == 'merizo' && !_useWriteCommandsDefault()) {
            args = args.slice(1);
            args.unshift(progName, '--useLegacyWriteOps');
        }

        return _startMerizoProgram.apply(null, args);
    };

    myPort = function() {
        var m = db.getMerizo();
        if (m.host.match(/:/))
            return m.host.match(/:(.*)/)[1];
        else
            return 27017;
    };

}());
