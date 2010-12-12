/* 
   test durability
*/

var testname = "dur1";
var step = 1;
var conn = null;

function log(str) {
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
    //    d.a.insert({ _id: 3, x: 22, y: [1, 2, 3] });
    //    d.a.insert({ _id: 4, x: 22, y: [1, 2, 3] });
    /*
    d.a.update({ _id: 4 }, { $inc: { x: 1} });
    d.a.ensureIndex({ x: 1 });
    d.a.update({ _id: 4 }, { $inc: { x: 1} });
    d.a.reIndex();
    */

    // assure writes applied in case we kill -9 on return from this function
    d.getLastError(); 

    log("endwork");
}

function verify() { 
    log("verify");
    var d = conn.getDB("test");
    print("count:" + d.foo.count());
    assert(d.foo.count() == 2);
}

log();

// directories
var path1 = testname+"nodur";
var path2 = testname+"dur";

// non-durable version
log();
conn = startMongodEmpty("--port", 30000, "--dbpath", path1, "--smallfiles");
work();
stopMongod(30000);

// durable version
log();
conn = startMongodEmpty("--port", 30001, "--dbpath", path2, "--dur", "--smallfiles");
work();

// wait for group commit.  use getLastError(...) later when that is enhanced.
sleep(400);

// kill the process hard
stopMongod(30001, /*signal*/9);

// journal file should be present, and non-empty as we killed hard

// restart and recover
log();
conn = startMongodNoReset("--port", 30002, "--dbpath", path2, "--dur", "--smallfiles");
verify();

log("stop");
stopMongod(30002);

// at this point, after clean shutdown, there should be no journal files
log("check no journal files");
assert(ls(path2 + "/journal") == null);

log("check data matches");
var diff = run("diff", path1 + "/test.ns", path2 + "/test.ns");
assert(diff == "", "error test.ns files differ");

log("check data matches 2");
var diff = run("diff", path1 + "/test.0", path2 + "/test.0");
assert(diff == "", "error test.0 files differ");

log("check data matches done");

print(testname + " SUCCESS");

