/* durability test dropping a database
*/

var debugging = false;
var testname = "dropdb";
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
    if (str)
        print("\n" + testname + " step " + step++ + " " + str);
    else
        print("\n" + testname + " step " + step++);
}

// if you do inserts here, you will want to set _id.  otherwise they won't match on different 
// runs so we can't do a binary diff of the resulting files to check they are consistent.
function work() {
    log("work (add data, drop database)");

    var e = conn.getDB("teste");
    e.foo.insert({ _id: 99 });

    var d = conn.getDB("test");
    d.foo.insert({ _id: 3, x: 22 });
    d.bar.insert({ _id: 3, x: 22 });

    d.dropDatabase();

    d.foo.insert({ _id: 100 });

    // assure writes applied in case we kill -9 on return from this function
    assert(d.runCommand({ getlasterror: 1, fsync: 1 }).ok, "getlasterror not ok");
}

function verify() {
    log("verify");
    var d = conn.getDB("test");
    var count = d.foo.count();
    if (count != 1) {
	    print("going to fail, test.foo.count() != 1 in verify()"); 
        sleep(10000); // easier to read the output this way
        print("\n\n\ndropdb.js FAIL test.foo.count() should be 1 but is : " + count);
	    print(d.foo.count() + "\n\n\n");
        assert(false);
    }
    assert(d.foo.findOne()._id == 100, "100");

    print("dropdb.js teste.foo.findOne:");
    printjson(conn.getDB("teste").foo.findOne());

    var teste = conn.getDB("teste");
    var testecount = teste.foo.count();
    if (testecount != 1) {
        print("going to fail, teste.foo.count() != 1 in verify()");
        sleep(10000); // easier to read the output this way
        print("\n\n\ndropdb.js FAIL teste.foo.count() should be 1 but is : " + testecount);
        print("\n\n\n");
        assert(false);
    } 
    print("teste.foo.count() = " + teste.foo.count());
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

// directories
var path1 = "/data/db/" + testname + "nodur";
var path2 = "/data/db/" + testname + "dur";

// non-durable version
log("mongod nodur");
conn = startMongodEmpty("--port", 30000, "--dbpath", path1, "--nodur", "--smallfiles");
work();
verify();
stopMongod(30000);

// durable version
log("mongod dur");
conn = startMongodEmpty("--port", 30001, "--dbpath", path2, "--dur", "--smallfiles", "--durOptions", 8);
work();
verify();

// kill the process hard
log("kill 9");
stopMongod(30001, /*signal*/9);

// journal file should be present, and non-empty as we killed hard

// we will force removal of a datafile to be sure we can recreate everything.
removeFile(path2 + "/test.0");
// the trick above is only valid if journals haven't rotated out, and also if lsn isn't skipping
removeFile(path2 + "/lsn");

log("restart and recover");
conn = startMongodNoReset("--port", 30002, "--dbpath", path2, "--dur", "--smallfiles", "--durOptions", 9);

log("verify after recovery");
verify();

log("stop mongod 30002");
stopMongod(30002);
sleep(5000);

// at this point, after clean shutdown, there should be no journal files
log("check no journal files");
checkNoJournalFiles(path2 + "/journal");

log("check data matches ns");
var diff = runDiff(path1 + "/test.ns", path2 + "/test.ns");
if (diff != "") {
    print("\n\n\nDIFFERS\n");
    print(diff);
}
assert(diff == "", "error test.ns files differ");

log("check data matches .0");
diff = runDiff(path1 + "/test.0", path2 + "/test.0");
if (diff != "") {
    print("\n\n\nDIFFERS\n");
    print(diff);
}
assert(diff == "", "error test.0 files differ");

log("check data matches done");

print(testname + " SUCCESS");

