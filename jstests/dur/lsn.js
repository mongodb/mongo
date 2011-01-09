/* test durability, specifically last sequence number function
   runs mongod, kill -9's, recovers
   then writes more data and verifies with DurParanoid that it matches
*/

var debugging = false;
var testname = "lsn";
var step = 1;
var conn = null;

var start = new Date();
function howLongSecs() {
    return (new Date() - start) / 1000;
}

function log(str) {
    if(str)
        print("\n" + testname+" step " + step++ + " " + str);
    else
        print(testname+" step " + step++);
}

function verify() {
    log("verify");
    var d = conn.getDB("test");
    var mycount = d.foo.count();
    print("count:" + mycount);
    assert(mycount>2, "count wrong");
}

// if you do inserts here, you will want to set _id.  otherwise they won't match on different 
// runs so we can't do a binary diff of the resulting files to check they are consistent.
function work() {
    log("work");
    x = 'x'; while(x.length < 1024) x+=x;
    var d = conn.getDB("test");
    d.foo.drop();
    d.foo.insert({});

    // go long enough we will have time to kill it later during recovery
    var j = 2;
    var MaxTime = 15;
    if (Math.random() < 0.05) {
        print("doing a longer pass");
        MaxTime = 90;
    }
    while (1) {
        d.foo.insert({ _id: j, z: x });
        d.foo.update({ _id: j }, { $inc: { a: 1} });
        if (j % 25 == 0)
            d.foo.remove({ _id: j });
        j++;
        if( j % 3 == 0 )
            d.foo.update({ _id: j }, { $inc: { a: 1} }, true);
        if (j % 10000 == 0)
            print(j);
        if (howLongSecs() > MaxTime)
            break;
    }

    verify();
    d.runCommand({ getLastError: 1, fsync: 1 });
}

if( debugging ) {
    // mongod already running in debugger
    print("DOING DEBUG MODE BEHAVIOR AS 'db' IS DEFINED -- RUN mongo --nodb FOR REGULAR TEST BEHAVIOR");
    conn = db.getMongo();
    work();
    sleep(30000);
    quit();
}

// directories
var path2 = "/data/db/" + testname+"dur";

// run mongod with a short --syncdelay to make LSN writing sooner
log("run mongod --dur and a short --syncdelay");
conn = startMongodEmpty("--syncdelay", 2, "--port", 30001, "--dbpath", path2, "--dur", "--smallfiles", "--durOptions", /*DurParanoid*/8, "--master", "--oplogSize", 64);
work();

log("wait a while for a sync and an lsn write");
sleep(14); // wait for lsn write

log("kill mongod -9");
stopMongod(30001, /*signal*/9);

// journal file should be present, and non-empty as we killed hard

// check that there is an lsn file
{
    var files = listFiles(path2 + "/journal/");
    assert(files.some(function (f) { return f.name.indexOf("lsn") >= 0; }),
           "lsn.js FAIL no lsn file found after kill, yet one is expected");
}
/*assert.soon(
    function () {
        var files = listFiles(path2 + "/journal/");
        return files.some(function (f) { return f.name.indexOf("lsn") >= 0; });
    },
    "lsn.js FAIL no lsn file found after kill, yet one is expected"
);*/

// restart and recover
log("restart mongod, recover, verify");
conn = startMongodNoReset("--port", 30002, "--dbpath", path2, "--dur", "--smallfiles", "--durOptions", 24, "--master", "--oplogSize", 64);
verify();

// idea here is to verify (in a simplistic way) that we are in a good state to do further ops after recovery
log("add data after recovery");
{
    var d = conn.getDB("test");
    d.xyz.insert({ x: 1 });
    d.xyz.insert({ x: 1 });
    d.xyz.insert({ x: 1 });
    d.xyz.update({}, { $set: { x: "aaaaaaaaaaaa"} });
    d.xyz.reIndex();
    d.xyz.drop();
    sleep(1);
    d.xyz.insert({ x: 1 });
}

log("stop mongod 30002");
stopMongod(30002);

print(testname + " SUCCESS");
