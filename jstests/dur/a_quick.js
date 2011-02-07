/* quick.js
   test durability
   this file should always run quickly
   other tests can be slow
*/

testname = "a_quick";
load("jstests/_tst.js");

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

// directories
var path1 = "/data/db/quicknodur";
var path2 = "/data/db/quickdur";

// non-durable version
tst.log("start mongod without dur");
var conn = startMongodEmpty("--port", 30000, "--dbpath", path1, "--nodur");
tst.log("without dur work");
var d = conn.getDB("test");
d.foo.insert({ _id:123 });
d.getLastError();
tst.log("stop without dur");
stopMongod(30000);

// durable version
tst.log("start mongod with dur");
conn = startMongodEmpty("--port", 30001, "--dbpath", path2, "--dur", "--durOptions", 8);
tst.log("with dur work");
d = conn.getDB("test");
d.foo.insert({ _id: 123 });
d.getLastError(); // wait

// we could actually do getlasterror fsync:1 now, but maybe this is agood 
// as it will assure that commits happen on a timely basis.  a bunch of the other dur/*js
// tests use fsync
tst.log("sleep a bit for a group commit");
sleep(8000);

// kill the process hard
tst.log("kill -9 mongod");
stopMongod(30001, /*signal*/9);

// journal file should be present, and non-empty as we killed hard

// we will force removal of a datafile to be sure we can recreate everything
// without it being present.
removeFile(path2 + "/test.0");

// for that to work, we can't skip anything though:
removeFile(path2 + "/journal/lsn");

// with the file deleted, we MUST start from the beginning of the journal.
// thus this check to be careful
var files = listFiles(path2 + "/journal/");
if (files.some(function (f) { return f.name.indexOf("lsn") >= 0; })) {
    print("\n\n\n");
    print(path2);
    printjson(files);
    assert(false, "a journal/lsn file is present which will make this test potentially fail.");
}

// restart and recover
tst.log("restart and recover");
conn = startMongodNoReset("--port", 30002, "--dbpath", path2, "--dur", "--durOptions", 9);
tst.log("check data results");
d = conn.getDB("test");

var countOk = (d.foo.count() == 1);
if (!countOk) {
    print("\n\n\na_quick.js FAIL count " + d.foo.count() + " is wrong\n\n\n");
    // keep going - want to see if the diff matches.  if so the sleep() above was too short?
}

tst.log("stop");
stopMongod(30002);

// at this point, after clean shutdown, there should be no journal files
tst.log("check no journal files");
checkNoJournalFiles(path2 + "/journal");

tst.log("check data matches");
var diff = tst.diff(path1 + "/test.ns", path2 + "/test.ns");
print("diff of .ns files returns:" + diff);

function showfiles() {
    print("\n\nERROR: files for dur and nodur do not match");
    print(path1 + " files:");
    printjson(listFiles(path1));
    print(path2 + " files:");
    printjson(listFiles(path2));
    print();
}

if (diff != "") {
    showfiles();    
    assert(diff == "", "error test.ns files differ");
}

diff = tst.diff(path1 + "/test.0", path2 + "/test.0");
print("diff of .0 files returns:" + diff);
if (diff != "") {
    showfiles();
    assert(diff == "", "error test.0 files differ");
}

assert(countOk, "a_quick.js document count after recovery was not the expected value");

tst.success();   
