/*
   test durability
*/

var debugging = false;
var testname = "dur1";
var step = 1;
var conn = null;

function checkNoJournalFiles(path, pass) {
    var files = listFiles(path);
    if (files.some(function(f) {
            return f.name.indexOf("prealloc") < 0;
        })) {
        if (pass == null) {
            // wait a bit longer for mongod to potentially finish if it is still running.
            sleep(10000);
            return checkNoJournalFiles(path, 1);
        }
        print("\n\n\n");
        print("FAIL path:" + path);
        print("unexpected files:");
        printjson(files);
        assert(false, "FAIL a journal/lsn file is present which is unexpected");
    }
}

function runDiff(a, b) {
    function reSlash(s) {
        var x = s;
        if (_isWindows()) {
            while (1) {
                var y = x.replace('/', '\\');
                if (y == x)
                    break;
                x = y;
            }
        }
        return x;
    }
    a = reSlash(a);
    b = reSlash(b);
    print("diff " + a + " " + b);
    return run("diff", a, b);
}

function log(str) {
    print();
    if (str)
        print(testname + " step " + step++ + " " + str);
    else
        print(testname + " step " + step++);
}

// if you do inserts here, you will want to set _id.  otherwise they won't match on different
// runs so we can't do a binary diff of the resulting files to check they are consistent.
function work() {
    log("work");
    var d = conn.getDB("test");
    d.foo.insert({_id: 3, x: 22});
    d.foo.insert({_id: 4, x: 22});
    d.a.insert({_id: 3, x: 22, y: [1, 2, 3]});
    d.a.insert({_id: 4, x: 22, y: [1, 2, 3]});
    d.a.update({_id: 4}, {$inc: {x: 1}});

    // try building an index.  however, be careful as object id's in system.indexes would vary, so
    // we do it manually:
    d.system.indexes.insert({_id: 99, ns: "test.a", key: {x: 1}, name: "x_1"});

    log("endwork");
    return d;
}

function verify() {
    log("verify test.foo.count == 2");
    var d = conn.getDB("test");
    var ct = d.foo.count();
    if (ct != 2) {
        print("\n\n\nFAIL dur1.js count is wrong in verify(): " + ct + "\n\n\n");
        assert(ct == 2);
    }
}

if (debugging) {
    // mongod already running in debugger
    conn = db.getMongo();
    work();
    sleep(30000);
    quit();
}

log();

// directories
var path1 = MongoRunner.dataPath + testname + "nodur";
var path2 = MongoRunner.dataPath + testname + "dur";

// non-durable version
log("run mongod without journaling");
conn = MongoRunner.runMongod({dbpath: path1, nodur: "", smallfiles: ""});
work();
MongoRunner.stopMongod(conn);

// durable version
log("run mongod with --journal");
conn = MongoRunner.runMongod({dbpath: path2, journal: "", smallfiles: "", journalOptions: 8});
work();

// wait for group commit.
printjson(conn.getDB('admin').runCommand({getlasterror: 1, fsync: 1}));

// kill the process hard
log("kill 9");
MongoRunner.stopMongod(conn.port, /*signal*/ 9);

// journal file should be present, and non-empty as we killed hard

// restart and recover
log("restart mongod --journal and recover");
conn = MongoRunner.runMongod({
    restart: true,
    cleanData: false,
    dbpath: path2,
    journal: "",
    smallfiles: "",
    journalOptions: 8
});
verify();

log("stop mongod");
MongoRunner.stopMongod(conn);

// stopMongod seems to be asynchronous (hmmm) so we sleep here.
// sleep(5000);

// at this point, after clean shutdown, there should be no journal files
log("check no journal files (after presumably clean shutdown)");
checkNoJournalFiles(path2 + "/journal");

log("check data matches ns");
var diff = runDiff(path1 + "/test.ns", path2 + "/test.ns");
if (diff != "") {
    print("\n\n\nDIFFERS\n");
    print(diff);
}
assert(diff == "", "error test.ns files differ");

log("check data matches .0");
var diff = runDiff(path1 + "/test.0", path2 + "/test.0");
if (diff != "") {
    print("\n\n\nDIFFERS\n");
    print(diff);
}
assert(diff == "", "error test.0 files differ");

log("check data matches done");

print(testname + " SUCCESS");
