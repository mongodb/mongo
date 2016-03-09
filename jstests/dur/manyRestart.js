/*
   test durability
*/

var debugging = false;
var testname = "manyRestarts";
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

function addRows() {
    var rand = Random.randInt(10000);
    log("add rows " + rand);
    var d = conn.getDB("test");
    for (var j = 0; j < rand; ++j) {
        d.rows.insert({a: 1, b: "blah"});
    }
    return rand;
}

function verify() {
    log("verify");
    var d = conn.getDB("test");
    assert.eq(d.foo.count(), 2, "collection count is wrong");
    assert.eq(d.a.count(), 2, "collection count is wrong");
}

function verifyRows(nrows) {
    log("verify rows " + nrows);
    var d = conn.getDB("test");
    assert.eq(d.rows.count(), nrows, "collection count is wrong");
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
log("starting first mongod");
conn = MongoRunner.runMongod({dbpath: path1, nojournal: "", smallfiles: ""});
work();
MongoRunner.stopMongod(conn);

// hail mary for windows
// Sat Jun 11 14:07:57 Error: boost::filesystem::create_directory: Access is denied:
// "\data\db\manyRestartsdur" (anon):1
sleep(1000);

log("starting second mongod");
conn = MongoRunner.runMongod({dbpath: path2, journal: "", smallfiles: "", journalOptions: 8});
work();
// wait for group commit.
printjson(conn.getDB('admin').runCommand({getlasterror: 1, fsync: 1}));

MongoRunner.stopMongod(conn);
sleep(5000);

for (var i = 0; i < 3; ++i) {
    // durable version
    log("restarting second mongod");
    conn = MongoRunner.runMongod({
        restart: true,
        cleanData: false,
        dbpath: path2,
        journal: "",
        smallfiles: "",
        journalOptions: 8
    });

    // wait for group commit.
    printjson(conn.getDB('admin').runCommand({getlasterror: 1, fsync: 1}));

    verify();

    // kill the process hard
    log("hard kill");
    MongoRunner.stopMongod(conn, /*signal*/ 9);

    sleep(5000);
}

// journal file should be present, and non-empty as we killed hard

// restart and recover
log("restart");
conn = MongoRunner.runMongod({
    restart: true,
    cleanData: false,
    dbpath: path2,
    journal: "",
    smallfiles: "",
    journalOptions: 8
});
log("verify");
verify();
log("stop");
MongoRunner.stopMongod(conn);
sleep(5000);

// at this point, after clean shutdown, there should be no journal files
log("check no journal files");
checkNoJournalFiles(path2 + "/journal");

log("check data matches ns");
var diff = runDiff(path1 + "/test.ns", path2 + "/test.ns");
assert(diff == "", "error test.ns files differ");

log("check data matches .0");
var diff = runDiff(path1 + "/test.0", path2 + "/test.0");
assert(diff == "", "error test.0 files differ");

log("check data matches done");

Random.setRandomSeed();
var nrows = 0;
for (var i = 0; i < 5; ++i) {
    // durable version
    log("restarting second mongod");
    conn = MongoRunner.runMongod({
        restart: true,
        cleanData: false,
        dbpath: path2,
        journal: "",
        smallfiles: "",
        journalOptions: 8
    });
    nrows += addRows();
    // wait for group commit.
    printjson(conn.getDB('admin').runCommand({getlasterror: 1, fsync: 1}));

    verifyRows(nrows);

    // kill the process hard
    log("hard kill");
    MongoRunner.stopMongod(conn, /*signal*/ 9);

    sleep(5000);
}

print(testname + " SUCCESS");
