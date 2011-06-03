/* 
   test durability option with tools (same a dur1.js but use mongorestore to do repair)
*/

var debugging = false;
var testname = "dur1_tool";
var step = 1;
var conn = null;

function checkNoJournalFiles(path, pass) {
    var files = listFiles(path);
    if (files.some(function (f) { return f.name.indexOf("prealloc") < 0; })) {
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
    if(str)
        print(testname+" step " + step++ + " " + str);
    else
        print(testname+" step " + step++);
}

// if you do inserts here, you will want to set _id.  otherwise they won't match on different 
// runs so we can't do a binary diff of the resulting files to check they are consistent.
function work() {
    log("work");
    var d = conn.getDB("test");
    d.foo.insert({ _id: 3, x: 22 });
    d.foo.insert({ _id: 4, x: 22 });
    d.a.insert({ _id: 3, x: 22, y: [1, 2, 3] });
    d.a.insert({ _id: 4, x: 22, y: [1, 2, 3] });
    d.a.update({ _id: 4 }, { $inc: { x: 1} });

    // try building an index.  however, be careful as object id's in system.indexes would vary, so we do it manually:
    d.system.indexes.insert({ _id: 99, ns: "test.a", key: { x: 1 }, name: "x_1", v: 0 });

//    d.a.update({ _id: 4 }, { $inc: { x: 1} });
//    d.a.reIndex();

    // assure writes applied in case we kill -9 on return from this function
    d.getLastError();

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

if( debugging ) { 
    // mongod already running in debugger
    conn = db.getMongo();
    work();
    sleep(30000);
    quit();
}

log();

// directories
var path1 = "/data/db/" + testname+"nodur";
var path2 = "/data/db/" + testname+"dur";

// non-durable version
log("run mongod without journaling");
conn = startMongodEmpty("--port", 30000, "--dbpath", path1, "--nodur", "--smallfiles");
work();
stopMongod(30000);

// durable version
log("run mongod with --journal");
conn = startMongodEmpty("--port", 30001, "--dbpath", path2, "--journal", "--smallfiles", "--journalOptions", 8);
work();

// wait for group commit.
printjson(conn.getDB('admin').runCommand({getlasterror:1, fsync:1}));

// kill the process hard
log("kill 9");
stopMongod(30001, /*signal*/9);

// journal file should be present, and non-empty as we killed hard

// mongorestore with --dbpath and --journal options should do a recovery pass
// empty.bson is an empty file so it won't actually insert anything
log("use mongorestore to recover");
runMongoProgram("mongorestore", "--dbpath", path2, "--journal", "-d", "test", "-c", "empty", "jstests/dur/data/empty.bson");

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

