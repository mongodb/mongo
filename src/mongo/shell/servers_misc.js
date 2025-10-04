function ToolTest(name, extraOptions) {
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
}

ToolTest.prototype.startDB = function (coll) {
    assert(!this.m, "db already running");

    let options = {port: this.port, dbpath: this.dbpath, bind_ip: "127.0.0.1"};

    Object.extend(options, this.options);

    this.m = startMongoProgram(...MongoRunner.arrOptions("mongod", options));
    this.db = this.m.getDB(this.baseName);
    if (coll) return this.db.getCollection(coll);
    return this.db;
};

ToolTest.prototype.stop = function () {
    if (!this.m) return;
    _stopMongoProgram(this.port);
    this.m = null;
    this.db = null;

    print("*** " + this.name + " completed successfully ***");
};

// Defer initializing these variables until the first call, as TestData attributes may be
// initialized as part of the --eval argument (e.g. by resmoke.py), which will not be evaluated
// until after this has loaded.
let maxPort;
let nextPort;

/**
 * Returns a port number that has not been given out to any other caller from the same mongo shell.
 */
function allocatePort() {
    // The default port was chosen in an attempt to have a large number of unassigned ports that
    // are also outside the ephemeral port range.
    nextPort ||= jsTestOptions().minPort || 20000;
    maxPort ||= jsTestOptions().maxPort || Math.pow(2, 16) - 1;

    if (nextPort === maxPort) {
        throw new Error("Exceeded maximum port range in allocatePort()");
    }
    return nextPort++;
}

/**
 * Resets the range of ports which have already been given out to callers of allocatePort().
 *
 * This function can be used to allow a test to allocate a large number of ports as part of starting
 * many MongoDB deployments without worrying about hitting the configured maximum. Callers of this
 * function should take care to ensure MongoDB deployments started earlier have been terminated and
 * won't be reused.
 */
function resetAllocatedPorts() {
    jsTest.log("Resetting the range of allocated ports");
    maxPort = nextPort = undefined;
}

let parallelShellPids = [];
function uncheckedParallelShellPidsString() {
    return parallelShellPids.join(", ");
}

function startParallelShell(jsCode, port, noConnect, ...optionArgs) {
    let shellPath = MongoRunner.getMongoShellPath();
    let args = [shellPath];

    if (typeof globalThis.db === "object") {
        if (!port) {
            // If no port override specified, just passthrough connect string.
            args.push("--host", globalThis.db.getMongo().host);
        } else {
            // Strip port numbers from connect string.
            const uri = new MongoURI(globalThis.db.getMongo().host);
            let connString = uri.servers
                .map(function (server) {
                    return server.host;
                })
                .join(",");
            if (uri.setName.length > 0) {
                connString = uri.setName + "/" + connString;
            }
            args.push("--host", connString);
        }
    }
    if (port) {
        args.push("--port", port);
    }

    // Convert function into call-string
    if (typeof jsCode == "function") {
        if (jsCode.constructor.name === "AsyncFunction") {
            jsCode = `await (${jsCode.toString()})();`;
        } else {
            jsCode = `(${jsCode.toString()})();`;
        }
    } else if (typeof jsCode == "string") {
        // do nothing
    } else {
        throw Error("bad first argument to startParallelShell");
    }

    if (noConnect) {
        args.push("--nodb");
    } else if (typeof globalThis.db == "object") {
        if (globalThis.db.getMongo().isGRPC()) {
            args.push("--gRPC");
        }
        jsCode = "db = db.getSiblingDB('" + globalThis.db.getName() + "');" + jsCode;
    }

    if (TestData) {
        jsCode = "TestData = " + tojson(TestData) + ";" + jsCode;
    }

    args.push(...optionArgs);
    args.push("--eval", jsCode);

    let pid = startMongoProgramNoConnect(...args);
    parallelShellPids.push(pid);

    // Returns a function that when called waits for the parallel shell to exit and returns the exit
    // code of the process. By default an error is thrown if the parallel shell exits with a nonzero
    // exit code.
    return function (options) {
        if (arguments.length > 0) {
            if (typeof options !== "object") {
                throw new Error("options must be an object");
            }
            if (options === null) {
                throw new Error("options cannot be null");
            }
        }
        let exitCode = waitProgram(pid);
        let pidIndex = parallelShellPids.indexOf(pid);
        parallelShellPids.splice(pidIndex);
        if (arguments.length === 0 || options.checkExitSuccess) {
            assert.eq(0, exitCode, "encountered an error in the parallel shell");
        }
        return exitCode;
    };
}

/**
 * Returns a list of 'numPorts' port numbers that have not been given out to any other caller from
 * the same mongo shell.
 */
function allocatePorts(numPorts) {
    let ports = [];
    for (let i = 0; i < numPorts; i++) {
        ports.push(allocatePort());
    }

    return ports;
}

let testingReplication = false;

export {
    ToolTest,
    allocatePort,
    allocatePorts,
    resetAllocatedPorts,
    startParallelShell,
    testingReplication,
    uncheckedParallelShellPidsString,
};
