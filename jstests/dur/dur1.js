/* 
   test durability
*/

var debugging = false;
var testname = "dur1";
var step = 1;
var conn = null;

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
}

function verify() { 
    log("verify");
    var d = conn.getDB("test");
    print("count:" + d.foo.count());
    assert(d.foo.count() == 2);
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
log();
conn = startMongodEmpty("--port", 30000, "--dbpath", path1, "--nodur", "--smallfiles");
work();
stopMongod(30000);

// durable version
log();
conn = startMongodEmpty("--port", 30001, "--dbpath", path2, "--dur", "--smallfiles", "--durOptions", 8);
work();

// wait for group commit.  use getLastError(...) later when that is enhanced.
sleep(400);

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
{
    if (ls(path2 + "/journal") != null) {
        // wait longer, stopMongod isn't synchronous
        sleep(8000);
        if (ls(path2 + "/journal") != null) {
            assert(false, "error seems to be journal files present after a clean mongod shutdown");
        }
    }
}

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

