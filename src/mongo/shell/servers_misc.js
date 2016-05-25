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

    var options = {
        port: this.port,
        dbpath: this.dbpath,
        nohttpinterface: "",
        noprealloc: "",
        smallfiles: "",
        bind_ip: "127.0.0.1"
    };

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

ReplTest = function(name, ports) {
    this.name = name;
    this.ports = ports || allocatePorts(2);
};

ReplTest.prototype.getPort = function(master) {
    if (master)
        return this.ports[0];
    return this.ports[1];
};

ReplTest.prototype.getPath = function(master) {
    var p = MongoRunner.dataPath + this.name + "-";
    if (master)
        p += "master";
    else
        p += "slave";
    return p;
};

ReplTest.prototype.getOptions = function(master, extra, putBinaryFirst, norepl) {

    if (!extra)
        extra = {};

    if (!extra.oplogSize)
        extra.oplogSize = "40";

    var a = [];
    if (putBinaryFirst)
        a.push("mongod");
    a.push("--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1", "--smallfiles");

    a.push("--port");
    a.push(this.getPort(master));

    a.push("--dbpath");
    a.push(this.getPath(master));

    if (jsTestOptions().noJournal && !('journal' in extra))
        a.push("--nojournal");
    if (jsTestOptions().noJournalPrealloc)
        a.push("--nopreallocj");
    if (jsTestOptions().keyFile) {
        a.push("--keyFile");
        a.push(jsTestOptions().keyFile);
    }

    if (!norepl) {
        if (master) {
            a.push("--master");
        } else {
            a.push("--slave");
            a.push("--source");
            a.push("127.0.0.1:" + this.ports[0]);
        }
    }

    for (var k in extra) {
        var v = extra[k];
        if (k in MongoRunner.logicalOptions)
            continue;
        a.push("--" + k);
        if (v != null && v !== "")
            a.push(v);
    }

    return a;
};

ReplTest.prototype.start = function(master, options, restart, norepl) {
    var lockFile = this.getPath(master) + "/mongod.lock";
    removeFile(lockFile);
    var o = this.getOptions(master, options, restart, norepl);

    if (restart) {
        var conn = startMongoProgram.apply(null, o);
        if (!master) {
            conn.setSlaveOk();
        }
        return conn;
    } else {
        var conn = _startMongod.apply(null, o);
        if (jsTestOptions().keyFile || jsTestOptions().auth) {
            jsTest.authenticate(conn);
        }
        if (!master) {
            conn.setSlaveOk();
        }
        return conn;
    }
};

ReplTest.prototype.stop = function(master, signal) {
    if (arguments.length == 0) {
        this.stop(true);
        this.stop(false);
        return;
    }

    print('*** ' + this.name + " completed successfully ***");
    return _stopMongoProgram(this.getPort(master), signal || 15);
};

/**
 * Returns a port number that has not been given out to any other caller from the same mongo shell.
 */
allocatePort = (function() {
    // Defer initializing these variables until the first call, as TestData attributes may be
    // initialized as part of the --eval argument (e.g. by resmoke.py), which will not be evaluated
    // until after this has loaded.
    var maxPort;
    var nextPort;

    return function() {
        // The default port was chosen in an attempt to have a large number of unassigned ports that
        // are also outside the ephemeral port range.
        nextPort = nextPort || jsTestOptions().minPort || 20000;
        maxPort = maxPort || jsTestOptions().maxPort || Math.pow(2, 16) - 1;

        if (nextPort === maxPort) {
            throw new Error("Exceeded maximum port range in allocatePort()");
        }
        return nextPort++;
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

function startParallelShell(jsCode, port, noConnect) {
    var args = ["mongo"];

    if (typeof db == "object") {
        var hostAndPort = db.getMongo().host.split(':');
        var host = hostAndPort[0];
        args.push("--host", host);
        if (!port && hostAndPort.length >= 2) {
            var port = hostAndPort[1];
        }
    }
    if (port) {
        args.push("--port", port);
    }

    // Convert function into call-string
    if (typeof(jsCode) == "function") {
        jsCode = "(" + jsCode.toString() + ")();";
    } else if (typeof(jsCode) == "string") {
    }
    // do nothing
    else {
        throw Error("bad first argument to startParallelShell");
    }

    if (noConnect) {
        args.push("--nodb");
    } else if (typeof(db) == "object") {
        jsCode = "db = db.getSiblingDB('" + db.getName() + "');" + jsCode;
    }

    if (TestData) {
        jsCode = "TestData = " + tojson(TestData) + ";" + jsCode;
    }

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
