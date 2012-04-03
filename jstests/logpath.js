// check replica set authentication

var name = "logpath";
var token = "logpath_token";

var dbdir = "/data/db/" + name + "/"; // this will work under windows as well as linux
var basedir = "/data/db/" + name + "files" + "/";
var logdir = basedir + "logdir/";
var testdir = basedir + "testdir/"
var sfile = _isWindows() ? "NUL:" : "/dev/null";

var logs = [token + "1", token + "2"];
var port = allocatePorts(6);

print("------ Creating directories");

// ensure log directory exists
assert(mkdir(basedir));
assert(mkdir(logdir));
assert(mkdir(testdir));

var cleanupFiles = function() { 
    var files = listFiles(logdir);

    for(f in files) {
        var name = files[f].name;

        // mostly here for safety
        if(name.indexOf(token) != -1) {
          removeFile(name);
        }
    }
};

var logCount = function(fpattern, prefix) {
    var files = listFiles(logdir);
    var pat = RegExp(fpattern + (prefix ? "" : "$"));
    var cnt = 0;

    for(f in files) {
        if(pat.test(files[f].name)) {
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
var m = MongoRunner.runMongod({ port: port[0], dbpath: dbdir, logpath: logdir + logs[0]});

// log should now exist (and no rotations should exist)
assert.eq(logCount(logs[0], true), 1);
stopMongod(port[0]);

print("------ Start mongod with logpath set to existing file");
m = MongoRunner.runMongod({ port: port[1], dbpath: dbdir, logpath: logdir + logs[0]});

// log should continue to exist
assert.eq(logCount(logs[0]), 1);

// but now there should be a rotation file
assert.eq(logCount(logs[0], true), 2);
cleanupFiles();

stopMongod(port[1]);

// Blocking on SERVER-5117:
// MongoRunner currently hangs if mongod fails to start so these tests don't work
if ( false ) {
    // only run forking test on *nix (not supported on Windows)
    if ( _isWindows() ) {
        print("------ Skipping fork tests... (Windows)");

    } else {
        print("------ Start mongod with logpath set to new file, fork");
        var m = MongoRunner.runMongod({ port: port[2], dbpath: dbdir, logpath: logdir + logs[1], fork: true});
      
        // log should now exist (and no rotations should exist)
        assert.eq(logCount(logs[1], true), 1);
        stopMongod(port[2]);
      
        print("------ Start mongod with logpath set to existing file, fork");
        m = MongoRunner.runMongod({ port: port[3], dbpath: dbdir, logpath: logdir + logs[1], fork: true});
      
        // log should continue to exist
        assert.eq(logCount(logs[1]), 1);
      
        // but now there should be a rotation file
        assert.eq(logCount(logs[1], true), 2);
        cleanupFiles();
      
        stopMongod(port[3]);
    }
    
    // the following tests depend on undefined behavior; assume that MongoRunner raises exception on error
    print("------ Confirm that launch fails with directory");
    assert.throws(function() { MongoRunner.runMongod({ port: port[4], dbpath: dbdir, logpath: testdir }); });

    print("------ Confirm that launch fails with special file");
    assert.throws(function() { MongoRunner.runMongod({ port: port[5], dbpath: dbdir, logpath: sfile }); });
}
