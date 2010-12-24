/* 
   test durability
*/

var debugging = false;
var testname = "oplog";
var step = 1;
var conn = null;

function log(str) {
    print();
    if(str)
        print(testname+" step " + step++ + " " + str);
    else
        print(testname+" step " + step++);
}

function verify() {
    log("verify");
    var d = conn.getDB("local");
    var mycount = d.oplog.$main.find({ "o.z": 3 }).count();
    print(mycount);
    assert(mycount == 3, "oplog doesnt match");
}

// if you do inserts here, you will want to set _id.  otherwise they won't match on different 
// runs so we can't do a binary diff of the resulting files to check they are consistent.
function work() {
    log("work");
    var d = conn.getDB("test");
    var q = conn.getDB("testq"); // use tewo db's to exercise JDbContext a bit.
    d.foo.insert({ _id: 3, x: 22 });
    d.foo.insert({ _id: 4, x: 22 });
    q.foo.insert({ _id: 4, x: 22 });
    d.a.insert({ _id: 3, x: 22, y: [1, 2, 3] });
    q.a.insert({ _id: 3, x: 22, y: [1, 2, 3] });
    d.a.insert({ _id: 4, x: 22, y: [1, 2, 3] });
    d.a.update({ _id: 4 }, { $inc: { x: 1} });
    // OpCode_ObjCopy fires on larger operations so make one that isn't tiny
    var big = "axxxxxxxxxxxxxxb";
    big = big + big;
    big = big + big;
    big = big + big;
    big = big + big;
    big = big + big;
    d.foo.insert({ _id: 5, q: "aaaaa", b: big, z: 3 });
    q.foo.insert({ _id: 5, q: "aaaaa", b: big, z: 3 });
    d.foo.insert({ _id: 6, q: "aaaaa", b: big, z: 3 });
    d.foo.update({ _id: 5 }, { $set: { z: 99} });

    // assure writes applied in case we kill -9 on return from this function
    d.getLastError();

    log("endwork");

    verify();
}

if( debugging ) {
    // mongod already running in debugger
    print("DOING DEBUG MODE BEHAVIOR AS 'db' IS DEFINED -- RUN mongo --nodb FOR REGULAR TEST BEHAVIOR");
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
log();
conn = startMongodEmpty("--port", 30000, "--dbpath", path1, "--nodur", "--smallfiles", "--master", "--oplogSize", 64);
work();
stopMongod(30000);

// durable version
log();
conn = startMongodEmpty("--port", 30001, "--dbpath", path2, "--dur", "--smallfiles", "--durOptions", /*DurParanoid*/8, "--master", "--oplogSize", 64);
work();

// wait for group commit.  use getLastError(...) later when that is enhanced.
sleep(400);

// kill the process hard
stopMongod(30001, /*signal*/9);

// journal file should be present, and non-empty as we killed hard

// restart and recover
log();
conn = startMongodNoReset("--port", 30002, "--dbpath", path2, "--dur", "--smallfiles", "--durOptions", 8, "--master", "--oplogSize", 64);
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
diff = run("diff", path1 + "/test.0", path2 + "/test.0");
if (diff != "") {
    print("\n\n\nDIFFERS\n");
    print(diff);
}
assert(diff == "", "error test.0 files differ");

log("check data matches done");

print(testname + " SUCCESS");

