/**
 * Test md5 validation of journal file.
 * This test is dependent on the journal file format and may require an update if the format changes,
 * see comments near fuzzFile() below.
 */

var debugging = false;
var testname = "dur_md5";
var step = 1;
var conn = null;

function log(str) {
    print();
    if(str)
        print(testname+" step " + step++ + " " + str);
    else
        print(testname+" step " + step++);
}

/** Changes here may require updating the byte index of the md5 hash, see File comments below. */
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

if( debugging ) { 
    // mongod already running in debugger
    conn = db.getMongo();
    work();
    sleep(30000);
    quit();
}

log();

var path = "/data/db/" + testname+"dur";

log();
conn = startMongodEmpty("--port", 30001, "--dbpath", path, "--dur", "--smallfiles", "--durOptions", 8);
work();

// wait for group commit.
printjson(conn.getDB('admin').runCommand({getlasterror:1, fsync:1}));

log("kill -9");

// kill the process hard
stopMongod(30001, /*signal*/9);

// journal file should be present, and non-empty as we killed hard

// Bit flip the first byte of the md5sum contained within the opcode footer.
// This ensures we get an md5 exception instead of some other type of exception.
var file = path + "/journal/j._0";

// if test fails, uncomment these "cp" lines to debug:
// run("cp", file, "/tmp/before");

// journal header is 8192
// jsectheader is 20
// so a little beyond that
fuzzFile(file, 8214+8);

// run("cp", file, "/tmp/after");

log("run mongod again recovery should fail");

// 100 exit code corresponds to EXIT_UNCAUGHT, which is triggered when there is an exception during recovery.
// 14 is is sometimes triggered instead due to SERVER-2184
exitCode = runMongoProgram( "mongod", "--port", 30002, "--dbpath", path, "--dur", "--smallfiles", "--durOptions", /*9*/13 );

if (exitCode != 100 && exitCode != 14) {
    print("\n\n\nFAIL md5.js expected mongod to fail but didn't? mongod exitCode: " + exitCode + "\n\n\n");
    // sleep a little longer to get more output maybe
    sleep(2000);
    assert(false);
}

// TODO Possibly we could check the mongod log to verify that the correct type of exception was thrown.  But
// that would introduce a dependency on the mongod log format, which we may not want.

print("SUCCESS md5.js");

// if we sleep a littler here we may get more out the mongod output logged
sleep(500);
