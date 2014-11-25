// This script tests the following behaviors:
//   - Creates a sharded collection (test.foo)
//   - Manually adds a split point
//   - Disables the balancer
//   - Inserts 10k documents and ensures they're evenly distributed
//   - Verifies a $where query can be killed on multiple DBs
//   - Tests fsync and fsync+lock permissions on sharded db

var s = new ShardingTest({shards: 2,
                          mongos: 1,
                          verbose:1,
                          other: {separateConfig: true}});
var db = s.getDB("test");   // db variable name is required due to startParallelShell()
var numDocs = 10000;
db.foo.drop();

// stop the balancer
s.stopBalancer()

// shard test.foo and add a split point
s.adminCommand({enablesharding: "test"});
s.adminCommand({shardcollection : "test.foo", key: {_id: 1}});
s.adminCommand({split : "test.foo", middle: {_id: numDocs/2}});

// move a chunk range to the non-primary shard
s.adminCommand({moveChunk: "test.foo", find: {_id: 3},
                to: s.getNonPrimaries("test")[0], _waitForDelete: true});

// restart balancer
s.setBalancer(true)

// insert 10k small documents into the sharded collection
for (i = 0; i < numDocs; i++)
    db.foo.insert({_id: i});

db.getLastError();
var x = db.foo.stats();

// verify the colleciton has been sharded and documents are evenly distributed
assert.eq("test.foo", x.ns, "namespace mismatch");
assert(x.sharded, "collection is not sharded");
assert.eq(numDocs, x.count, "total count");
assert.eq(numDocs / 2, x.shards.shard0000.count, "count on shard0000");
assert.eq(numDocs / 2, x.shards.shard0001.count, "count on shard0001");
assert(x.totalIndexSize > 0);
assert(x.numExtents > 0);

// insert one doc into a non-sharded collection
db.bar.insert({x: 1});
var x = db.bar.stats();
assert.eq(1, x.count, "XXX1");
assert.eq("test.bar", x.ns, "XXX2");
assert(!x.sharded, "XXX3: " + tojson(x));

// fork shell and start querying the data
var start = new Date();

// TODO:  Still potential problem when our sampling of current ops misses when $where is active -
// solution is to increase sleep time
var whereKillSleepTime = 10000;
var parallelCommand =
    "try { " +
    "    db.foo.find(function(){ " +
    "        sleep( " + whereKillSleepTime + " ); " +
    "        return false; " +
    "     }).itcount(); " +
    "} " +
    "catch(e) { " +
    "    print('PShell execution ended:'); " +
    "    printjson(e) " +
    "}";

// fork a parallel shell, but do not wait for it to start
print("about to fork new shell at: " + Date());
join = startParallelShell(parallelCommand);
print("done forking shell at: " + Date());

// Get all current $where operations
function getMine(printInprog) {
    var inprog = db.currentOp().inprog;
    if (printInprog)
        printjson(inprog);

    // Find all the where queries
    var mine = [];
    for (var x=0; x<inprog.length; x++) {
        if (inprog[x].query && inprog[x].query.$where) {
            mine.push(inprog[x]);
        }
    }

    return mine;
}

var curOpState = 0; // 0 = not found, 1 = killed
var killTime = null;
var i = 0;

assert.soon(function() {
    // Get all the current operations
    mine = getMine(true);  // SERVER-8794: print all operations
    // get curren tops, but only print out operations before we see a $where op has started
    // mine = getMine(curOpState == 0 && i > 20);
    i++;

    // Wait for the queries to start (one per shard, so 2 total)
    if (curOpState == 0 && mine.length == 2) {
        // queries started
        curOpState = 1;
        // kill all $where
        mine.forEach(function(z) {
            printjson(db.getSisterDB("admin").killOp(z.opid));
        });
        killTime = new Date();
    }
    // Wait for killed queries to end
    else if (curOpState == 1 && mine.length == 0) {
        // Queries ended
        curOpState = 2;
        return true;
    }

}, "Couldn't kill the $where operations.", 2 * 60 * 1000);

print("after loop: " + Date());
assert(killTime, "timed out waiting too kill last mine:" + tojson(mine));

assert.eq( 2 , curOpState , "failed killing" );

killTime = new Date().getTime() - killTime.getTime();
print("killTime: " + killTime);
print("time if run full: " + (numDocs * whereKillSleepTime));
assert.gt(whereKillSleepTime * numDocs / 20, killTime, "took too long to kill");

// wait for the parallel shell we spawned to complete
join();
var end = new Date();
print("elapsed: " + (end.getTime() - start.getTime()));

// test fsync command on non-admin db
x = db.runCommand("fsync");
assert(!x.ok , "fsync on non-admin namespace should fail : " + tojson(x));
assert(x.code == 13,
       "fsync on non-admin succeeded, but should have failed: " + tojson(x));

// test fsync on admin db
x = db._adminCommand("fsync");
assert(x.ok == 1 && x.numFiles > 0, "fsync failed: " + tojson(x));

// test fsync+lock on admin db
x = db._adminCommand({"fsync" :1, lock:true});
assert(!x.ok, "lock should fail: " + tojson(x));

// write back stuff
// SERVER-4194

function countWritebacks(curop) {
    print("---------------");
    var num = 0;
    for (var i = 0; i < curop.inprog.length; i++) {
        var q = curop.inprog[i].query;
        if (q && q.writebacklisten) {
            printjson(curop.inprog[i]);
            num++;
        }
    }
    return num;
}

x = db.currentOp();
assert.eq(0, countWritebacks(x), "without all");

x = db.currentOp(true);
y = countWritebacks(x);
assert(y == 1 || y == 2, "with all: " + y);

s.stop()
