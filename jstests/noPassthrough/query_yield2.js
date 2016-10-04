if (0) {  // Test disabled until SERVER-8579 is finished. Reminder ticket: SERVER-8342

    var currentOp;
    var N;
    var i;
    var t;
    var q;
    var len;
    var num;
    var start;
    var insertTime;

    load("jstests/libs/slow_weekly_util.js");
    testServer = new SlowWeeklyMongod("query_yield2");
    db = testServer.getDB("test");

    t = db.query_yield2;
    t.drop();

    N = 200;
    i = 0;

    q = function() {
        var x = this.n;
        for (var i = 0; i < 25000; i++) {
            x = x * 2;
        }
        return false;
    };

    print("Shell ==== Creating test.query_yield2 collection ...");
    print(
        "Shell ==== Adding documents until a time-wasting query takes over 2 seconds to complete");
    while (true) {
        function fill() {
            var bulk = t.initializeUnorderedBulkOp();
            for (; i < N; ++i) {
                bulk.insert({_id: i, n: 1});
            }
            assert.writeOK(bulk.execute());
        }
        function timeQuery() {
            return Date.timeFunc(function() {
                assert.eq(0, t.find(q).itcount());
            });
        }
        print("Shell ==== Adding document IDs from " + i + " to " + (N - 1));
        fill();
        print("Shell ==== Running warm-up query 1");
        timeQuery();
        print("Shell ==== Running warm-up query 2");
        timeQuery();
        print("Shell ==== Running timed query ...");
        time = timeQuery();
        print("Shell ==== Query across " + N + " documents took " + time + " ms");
        if (time > 2000) {
            print("Shell ==== Reached desired 2000 ms mark (at " + time +
                  " ms), proceding to next step");
            break;
        }
        N *= 2;
        print("Shell ==== Did not reach 2000 ms, increasing fill point to " + N + " documents");
    }

    print("Shell ==== Testing db.currentOp to make sure nothing is in progress");
    print("Shell ==== Dump of db.currentOp:");
    currentOp = db.currentOp();
    print(tojson(currentOp));
    len = currentOp.inprog.length;
    if (len) {
        print("Shell ==== This test is broken: db.currentOp().inprog.length is " + len);
        throw Error("query_yield2.js test is broken");
    }
    print("Shell ==== The test is working so far: db.currentOp().inprog.length is " + len);

    print("Shell ==== Starting parallel shell to test if slow query will yield to write");
    join = startParallelShell(
        "print( 0 == db.query_yield2.find( function(){ var x=this.n; for ( var i=0; i<50000; i++ ){ x = x * 2; } return false; } ).itcount() ); ");

    print("Shell ==== Waiting until db.currentOp().inprog becomes non-empty");
    assert.soon(function() {
        currentOp = db.currentOp();
        len = currentOp.inprog.length;
        if (len) {
            print("Shell ==== Wait satisfied: db.currentOp().inprog.length is " + len);
            print("Shell ==== Dump of db.currentOp:");
            print(tojson(currentOp));
            print("Shell ==== Checking if this currentOp is the query we are waiting for");
            if (currentOp.inprog[0].ns == "test.query_yield2" &&
                currentOp.inprog[0].query["$where"]) {
                print("Shell ==== Yes, we found the query we are waiting for");
                return true;
            }
            if (currentOp.inprog[0].ns == "" && currentOp.inprog[0].query["whatsmyuri"]) {
                print("Shell ==== No, we found a \"whatsmyuri\" query, waiting some more");
                return false;
            }
            print(
                "Shell ==== No, we found something other than our query or a \"whatsmyuri\", waiting some more");
            return false;
        }
        return len > 0;
    }, "Wait failed, db.currentOp().inprog never became non-empty", 2000, 1);

    print(
        "Shell ==== Now that we have seen db.currentOp().inprog show that our query is running, we start the real test");
    num = 0;
    start = new Date();
    while (((new Date()).getTime() - start) < (time * 2)) {
        if (num == 0) {
            print("Shell ==== Starting loop " + num + ", inserting 1 document");
        }
        insertTime = Date.timeFunc(function() {
            t.insert({x: 1});
        });
        currentOp = db.currentOp();
        len = currentOp.inprog.length;
        print("Shell ==== Time to insert document " + num + " was " + insertTime +
              " ms, db.currentOp().inprog.length is " + len);
        if (num++ == 0) {
            if (len != 1) {
                print("Shell ==== TEST FAILED!  db.currentOp().inprog.length is " + len);
                print("Shell ==== Dump of db.currentOp:");
                print(tojson(currentOp));
                throw Error("TEST FAILED!");
            }
        }
        assert.gt(200,
                  insertTime,
                  "Insert took too long (" + insertTime + " ms), should be less than 200 ms");
        if (currentOp.inprog.length == 0) {
            break;
        }
    }

    print("Shell ==== Finished inserting documents, reader also finished");
    print("Shell ==== Waiting for parallel shell to exit");
    join();

    currentOp = db.currentOp();
    len = currentOp.inprog.length;
    if (len != 0) {
        print("Shell ==== Final sanity check FAILED!  db.currentOp().inprog.length is " + len);
        print("Shell ==== Dump of db.currentOp:");
        print(tojson(currentOp));
        throw Error("TEST FAILED!");
    }
    print("Shell ==== Test completed successfully, shutting down server");
    testServer.stop();
}
