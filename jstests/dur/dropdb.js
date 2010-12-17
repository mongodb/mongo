/* 
durability test dropping a database
*/

var debugging = false;
var testname = "dropdb";
var step = 1;
var conn = null;

function log(str) {
    if (str)
        print(testname + " step " + step++ + " " + str);
    else
        print(testname + " step " + step++);
}

// if you do inserts here, you will want to set _id.  otherwise they won't match on different 
// runs so we can't do a binary diff of the resulting files to check they are consistent.
function work() {
    log("work");

    var e = conn.getDB("teste");
    e.foo.insert({ _id: 99 });

    var d = conn.getDB("test");
    d.foo.insert({ _id: 3, x: 22 });
    d.bar.insert({ _id: 3, x: 22 });

    d.dropDatabase();

    d.foo.insert({ _id: 100 });

    // assure writes applied in case we kill -9 on return from this function
    d.runCommand({ getlasterror: 1, fsync: 1 });

    log("endwork");
}

function verify() {
    log("verify");
    var d = conn.getDB("test");
    assert(d.foo.count() == 1,"count1");
    assert(d.foo.findOne()._id == 100, "100");

    print("\n\nteste:");
    printjson(conn.getDB("teste").foo.findOne());
    print();

    var teste = conn.getDB("teste");
    print("teste count " + teste.foo.count());
    assert(teste.foo.findOne()._id == 99, "teste");

}

if (debugging) {
    // mongod already running in debugger
    conn = db.getMongo();
    work();
    verify();
    sleep(30000);
    quit();
}

log();

// directories
var path1 = "/data/db/" + testname + "nodur";
var path2 = "/data/db/" + testname + "dur";

// non-durable version
log();
conn = startMongodEmpty("--port", 30000, "--dbpath", path1, "--nodur", "--smallfiles");
work();
verify();
stopMongod(30000);

// durable version
log();
conn = startMongodEmpty("--port", 30001, "--dbpath", path2, "--dur", "--smallfiles", "--durOptions", 8);
work();
verify();

// kill the process hard
stopMongod(30001, /*signal*/9);

// journal file should be present, and non-empty as we killed hard

// restart and recover
log();
conn = startMongodNoReset("--port", 30002, "--dbpath", path2, "--dur", "--smallfiles", "--durOptions", 8);
verify();

log("stop");
stopMongod(30002);

// stopMongod seems to be asynchronous (hmmm) so we sleep here.
sleep(5000);

// at this point, after clean shutdown, there should be no journal files
log("check no journal files");
assert(ls(path2 + "/journal") == null);

log("check data matches ns");
var diff = run("diff", path1 + "/test.ns", path2 + "/test.ns");
if (diff != "") {
    print("\n\n\nDIFFERS\n");
    print(diff);
}
assert(diff == "", "error test.ns files differ");

log("check data matches .0");
var diff = run("diff", path1 + "/test.0", path2 + "/test.0");
if (diff != "") {
    print("\n\n\nDIFFERS\n");
    print(diff);
}
assert(diff == "", "error test.0 files differ");

log("check data matches done");

print(testname + " SUCCESS");

