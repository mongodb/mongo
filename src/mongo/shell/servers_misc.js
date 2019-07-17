ToolTest = function(name, extraOptions) {
    this.name = name;
    this.options = extraOptions;
    this.port = allocatePort();
    this.baseName = "jstests_tool_" + name;
    this.root = MongoRunner.dataPath + this.baseName;
    this.dbpath = this.root + "/";
    this.ext = this.root + "_external/";
    this.extFile = this.root + "_external/a";
    resetDbpath(this.dbpath);
    resetDbpath(this.ext);
};

ToolTest.prototype.startDB = function(coll) {
    assert(!this.m, "db already running");

    var options = {port: this.port, dbpath: this.dbpath, bind_ip: "127.0.0.1"};

    Object.extend(options, this.options);

    this.m = startMongoProgram.apply(null, MongoRunner.arrOptions("mongod", options));
    this.db = this.m.getDB(this.baseName);
    if (coll)
        return this.db.getCollection(coll);
    return this.db;
};

ToolTest.prototype.stop = function() {
    if (!this.m)
        return;
    _stopMongoProgram(this.port);
    this.m = null;
    this.db = null;

    print('*** ' + this.name + " completed successfully ***");
};

ToolTest.prototype.runTool = function() {
    var a = ["mongo" + arguments[0]];

    var hasdbpath = false;
    var hasDialTimeout = false;

    for (var i = 1; i < arguments.length; i++) {
        a.push(arguments[i]);
        if (arguments[i] === "--dbpath")
            hasdbpath = true;
        if (arguments[i] === "--dialTimeout")
            hasDialTimeout = true;
    }

    if (!hasdbpath) {
        a.push("--host");
        a.push("127.0.0.1:" + this.port);
    }

    if (!hasDialTimeout) {
        a.push("--dialTimeout");
        a.push("30");
    }

    return runMongoProgram.apply(null, a);
};

/**
 * Returns a port number that has not been given out to any other caller from the same mongo shell.
 */
var allocatePort;

/**
 * Resets the range of ports which have already been given out to callers of allocatePort().
 *
 * This function can be used to allow a test to allocate a large number of ports as part of starting
 * many MongoDB deployments without worrying about hitting the configured maximum. Callers of this
 * function should take care to ensure MongoDB deployments started earlier have been terminated and
 * won't be reused.
 */
var resetAllocatedPorts;

(function() {
// Defer initializing these variables until the first call, as TestData attributes may be
// initialized as part of the --eval argument (e.g. by resmoke.py), which will not be evaluated
// until after this has loaded.
var maxPort;
var nextPort;

allocatePort = function() {
    // The default port was chosen in an attempt to have a large number of unassigned ports that
    // are also outside the ephemeral port range.
    nextPort = nextPort || jsTestOptions().minPort || 20000;
    maxPort = maxPort || jsTestOptions().maxPort || Math.pow(2, 16) - 1;

    if (nextPort === maxPort) {
        throw new Error("Exceeded maximum port range in allocatePort()");
    }
    return nextPort++;
};

resetAllocatedPorts = function() {
    jsTest.log("Resetting the range of allocated ports");
    maxPort = nextPort = undefined;
};
})();

/**
 * Returns a list of 'numPorts' port numbers that have not been given out to any other caller from
 * the same mongo shell.
 */
allocatePorts = function(numPorts) {
    var ports = [];
    for (var i = 0; i < numPorts; i++) {
        ports.push(allocatePort());
    }

    return ports;
};

function startParallelShell(jsCode, port, noConnect, ...optionArgs) {
    var shellPath = MongoRunner.mongoShellPath;
    var args = [shellPath];

    if (typeof db == "object") {
        if (!port) {
            // If no port override specified, just passthrough connect string.
            args.push("--host", db.getMongo().host);
        } else {
            // Strip port numbers from connect string.
            const uri = new MongoURI(db.getMongo().host);
            var connString = uri.servers
                                 .map(function(server) {
                                     return server.host;
                                 })
                                 .join(',');
            if (uri.setName.length > 0) {
                connString = uri.setName + '/' + connString;
            }
            args.push("--host", connString);
        }
    }
    if (port) {
        args.push("--port", port);
    }

    // Convert function into call-string
    if (typeof (jsCode) == "function") {
        jsCode = "(" + jsCode.toString() + ")();";
    } else if (typeof (jsCode) == "string") {
    }
    // do nothing
    else {
        throw Error("bad first argument to startParallelShell");
    }

    if (noConnect) {
        args.push("--nodb");
    } else if (typeof (db) == "object") {
        jsCode = "db = db.getSiblingDB('" + db.getName() + "');" + jsCode;
    }

    if (TestData) {
        jsCode = "TestData = " + tojson(TestData) + ";" + jsCode;
    }

    args.push(...optionArgs);
    args.push("--eval", jsCode);

    var pid = startMongoProgramNoConnect.apply(null, args);

    // Returns a function that when called waits for the parallel shell to exit and returns the exit
    // code of the process. By default an error is thrown if the parallel shell exits with a nonzero
    // exit code.
    return function(options) {
        if (arguments.length > 0) {
            if (typeof options !== "object") {
                throw new Error("options must be an object");
            }
            if (options === null) {
                throw new Error("options cannot be null");
            }
        }
        var exitCode = waitProgram(pid);
        if (arguments.length === 0 || options.checkExitSuccess) {
            assert.eq(0, exitCode, "encountered an error in the parallel shell");
        }
        return exitCode;
    };
}

var testingReplication = false;
