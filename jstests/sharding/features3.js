// This script tests the following behaviors:
//   - Creates a sharded collection (test.foo)
//   - Manually adds a split point
//   - Disables the balancer
//   - Inserts 10k documents and ensures they're evenly distributed
//   - Verifies a $where query can be killed on multiple DBs
//   - Tests fsync and fsync+lock permissions on sharded db
(function() {
    'use strict';

    var s = new ShardingTest({shards: 2, mongos: 1});
    var dbForTest = s.getDB("test");
    dbForTest.foo.drop();

    var numDocs = 10000;

    // shard test.foo and add a split point
    s.adminCommand({enablesharding: "test"});
    s.ensurePrimaryShard('test', 'shard0001');
    s.adminCommand({shardcollection: "test.foo", key: {_id: 1}});
    s.adminCommand({split: "test.foo", middle: {_id: numDocs / 2}});

    // move a chunk range to the non-primary shard
    s.adminCommand({
        moveChunk: "test.foo",
        find: {_id: 3},
        to: s.getNonPrimaries("test")[0],
        _waitForDelete: true
    });

    // restart balancer
    s.startBalancer();

    // insert 10k small documents into the sharded collection
    var bulk = dbForTest.foo.initializeUnorderedBulkOp();
    for (var i = 0; i < numDocs; i++) {
        bulk.insert({_id: i});
    }
    assert.writeOK(bulk.execute());

    var x = dbForTest.foo.stats();

    // verify the colleciton has been sharded and documents are evenly distributed
    assert.eq("test.foo", x.ns, "namespace mismatch");
    assert(x.sharded, "collection is not sharded");
    assert.eq(numDocs, x.count, "total count");
    assert.eq(numDocs / 2, x.shards.shard0000.count, "count on shard0000");
    assert.eq(numDocs / 2, x.shards.shard0001.count, "count on shard0001");
    assert(x.totalIndexSize > 0);

    // insert one doc into a non-sharded collection
    dbForTest.bar.insert({x: 1});
    var x = dbForTest.bar.stats();
    assert.eq(1, x.count, "XXX1");
    assert.eq("test.bar", x.ns, "XXX2");
    assert(!x.sharded, "XXX3: " + tojson(x));

    // fork shell and start querying the data
    var start = new Date();

    var whereKillSleepTime = 1000;
    var parallelCommand = "db.foo.find(function() { " + "    sleep(" + whereKillSleepTime + "); " +
        "    return false; " + "}).itcount(); ";

    // fork a parallel shell, but do not wait for it to start
    print("about to fork new shell at: " + Date());
    var awaitShell = startParallelShell(parallelCommand, s.s.port);
    print("done forking shell at: " + Date());

    // Get all current $where operations
    function getInProgWhereOps() {
        var inprog = dbForTest.currentOp().inprog;

        // Find all the where queries
        var myProcs = [];
        inprog.forEach(function(op) {
            if (op.query && op.query.filter && op.query.filter.$where) {
                myProcs.push(op);
            }
        });

        if (myProcs.length == 0) {
            print('No $where operations found: ' + tojson(inprog));
        } else {
            print('Found ' + myProcs.length + ' $where operations: ' + tojson(myProcs));
        }

        return myProcs;
    }

    var curOpState = 0;  // 0 = not found, 1 = killed
    var killTime = null;
    var mine;

    assert.soon(function() {
        // Get all the current operations
        mine = getInProgWhereOps();

        // Wait for the queries to start (one per shard, so 2 total)
        if (curOpState == 0 && mine.length == 2) {
            // queries started
            curOpState = 1;
            // kill all $where
            mine.forEach(function(z) {
                printjson(dbForTest.getSisterDB("admin").killOp(z.opid));
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

    assert.eq(2, curOpState, "failed killing");

    killTime = new Date().getTime() - killTime.getTime();
    print("killTime: " + killTime);
    print("time if run full: " + (numDocs * whereKillSleepTime));
    assert.gt(whereKillSleepTime * numDocs / 20, killTime, "took too long to kill");

    // wait for the parallel shell we spawned to complete
    var exitCode = awaitShell({checkExitSuccess: false});
    assert.neq(
        0, exitCode, "expected shell to exit abnormally due to JS execution being terminated");

    var end = new Date();
    print("elapsed: " + (end.getTime() - start.getTime()));

    // test fsync command on non-admin db
    x = dbForTest.runCommand("fsync");
    assert(!x.ok, "fsync on non-admin namespace should fail : " + tojson(x));
    assert(x.code == 13, "fsync on non-admin succeeded, but should have failed: " + tojson(x));

    // test fsync on admin db
    x = dbForTest._adminCommand("fsync");
    assert(x.ok == 1, "fsync failed: " + tojson(x));
    if (x.all.shard0000 > 0) {
        assert(x.numFiles > 0, "fsync failed: " + tojson(x));
    }

    // test fsync+lock on admin db
    x = dbForTest._adminCommand({"fsync": 1, lock: true});
    assert(!x.ok, "lock should fail: " + tojson(x));

    s.stop();

})();
