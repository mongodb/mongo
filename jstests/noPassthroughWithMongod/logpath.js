// check replica set authentication

let name = "logpath";
let token = "logpath_token";

let dbdir = MongoRunner.dataPath + name + "/"; // this will work under windows as well as linux
let basedir = MongoRunner.dataPath + name + "files" + "/";
let logdir = basedir + "logdir/";
let testdir = basedir + "testdir/";
let sfile = _isWindows() ? "NUL" : "/dev/null";

let logs = [token + "1", token + "2"];
let port = allocatePorts(6);

print("------ Creating directories");

// ensure log directory exists
assert(mkdir(basedir));
assert(mkdir(logdir));
assert(mkdir(testdir));

let cleanupFiles = function () {
    let files = listFiles(logdir);

    for (let f in files) {
        let name = files[f].name;

        // mostly here for safety
        if (name.indexOf(token) != -1) {
            removeFile(name);
        }
    }
};

let logCount = function (fpattern, prefix) {
    let files = listFiles(logdir);
    let pat = RegExp(fpattern + (prefix ? "" : "$"));
    let cnt = 0;

    for (let f in files) {
        if (pat.test(files[f].name)) {
            cnt++;
        }
    }

    return cnt;
};

print("------ Cleaning up old files");
cleanupFiles();

// log should not exist
assert.eq(logCount(logs[0]), 0);

print("------ Start mongod with logpath set to new file");
var m = MongoRunner.runMongod({port: port[0], dbpath: dbdir, logpath: logdir + logs[0]});

// log should now exist (and no rotations should exist)
assert.eq(logCount(logs[0], true), 1);
MongoRunner.stopMongod(m /*port[0]*/);

print("------ Start mongod with logpath set to existing file");
m = MongoRunner.runMongod({port: port[1], dbpath: dbdir, logpath: logdir + logs[0]});

// log should continue to exist
assert.eq(logCount(logs[0]), 1);

// but now there should be a rotation file
assert.eq(logCount(logs[0], true), 2);
cleanupFiles();

MongoRunner.stopMongod(m /*port[1]*/);

// Blocking on SERVER-5117:
// MongoRunner currently hangs if mongod fails to start so these tests don't work
if (false) {
    // only run forking test on *nix (not supported on Windows)
    if (_isWindows()) {
        print("------ Skipping fork tests... (Windows)");
    } else {
        print("------ Start mongod with logpath set to new file, fork");
        var m = MongoRunner.runMongod({port: port[2], dbpath: dbdir, logpath: logdir + logs[1], fork: true});

        // log should now exist (and no rotations should exist)
        assert.eq(logCount(logs[1], true), 1);
        MongoRunner.stopMongod(m /*port[2]*/);

        print("------ Start mongod with logpath set to existing file, fork");
        m = MongoRunner.runMongod({port: port[3], dbpath: dbdir, logpath: logdir + logs[1], fork: true});

        // log should continue to exist
        assert.eq(logCount(logs[1]), 1);

        // but now there should be a rotation file
        assert.eq(logCount(logs[1], true), 2);
        cleanupFiles();

        MongoRunner.stopMongod(m /*port[3]*/);
    }

    // the following tests depend on undefined behavior; assume that MongoRunner raises exception on
    // error
    print("------ Confirm that launch fails with directory");
    assert.throws(function () {
        MongoRunner.runMongod({port: port[4], dbpath: dbdir, logpath: testdir});
    });

    print("------ Confirm that launch fails with special file");
    assert.throws(function () {
        MongoRunner.runMongod({port: port[5], dbpath: dbdir, logpath: sfile});
    });
}
