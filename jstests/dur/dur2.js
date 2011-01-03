/* test durability
   runs mongod, kill -9's, recovers
*/

var debugging = false;
var testname = "dur2";
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

// durable version
log("run mongod with --dur");
conn = startMongodEmpty("--port", 30001, "--dbpath", path2, "--dur", "--smallfiles", "--durOptions", /*DurParanoid*/8, "--master", "--oplogSize", 64);
work();

// kill the process hard
stopMongod(30001, /*signal*/9);

// journal file should be present, and non-empty as we killed hard

// restart and recover
log("restart and recover");
conn = startMongodNoReset("--port", 30002, "--dbpath", path2, "--dur", "--smallfiles", "--durOptions", 8, "--master", "--oplogSize", 64);
verify();

log("stopping 30002");
stopMongod(30002);

// stopMongod seems to be asynchronous (hmmm) so we sleep here.
sleep(5000);

print(testname + " SUCCESS");

