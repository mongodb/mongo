// test rollback in replica sets

// try running as :
// 
//   mongo --nodb rollback.js | tee out | grep -v ^m31
//

var debugging = 0;

function ifReady(db, f) {
    var stats = db.adminCommand({ replSetGetStatus: 1 });
    

    // only eval if state isn't recovery
    if (stats && stats.myState != 3) {
        return f();
    }

    return false;
}

function pause(s) {
    print(s);
    while (debugging) {
        sleep(3000);
        print(s);
    }
}

function deb(obj) { 
    if( debugging ) {
        print("\n\n\n" + obj + "\n\n");
    }  
}

w = 0;

function wait(f) {
    w++;
    var n = 0;
    while (!f()) {
        if( n % 4 == 0 )
            print("rollback.js waiting " + w);
        if (++n == 4) {
            print("" + f);
        }
        assert(n < 200, 'tried 200 times, giving up');
        sleep(1000);
    }
}

doTest = function (signal) {

    var replTest = new ReplSetTest({ name: 'unicomplex', nodes: 3 });
    var nodes = replTest.nodeList();
    //print(tojson(nodes));

    var conns = replTest.startSet();
    var r = replTest.initiate({ "_id": "unicomplex",
        "members": [
                             { "_id": 0, "host": nodes[0] },
                             { "_id": 1, "host": nodes[1] },
                             { "_id": 2, "host": nodes[2], arbiterOnly: true}]
    });

    // Make sure we have a master
    var master = replTest.getMaster();
    a_conn = conns[0];
    A = a_conn.getDB("admin");
    b_conn = conns[1];
    a_conn.setSlaveOk();
    b_conn.setSlaveOk();
    B = b_conn.getDB("admin");
    assert(master == conns[0], "conns[0] assumed to be master");
    assert(a_conn == master);

    //deb(master);

    // Make sure we have an arbiter
    assert.soon(function () {
        res = conns[2].getDB("admin").runCommand({ replSetGetStatus: 1 });
        return res.myState == 7;
    }, "Arbiter failed to initialize.");

    // Wait for initial replication
    var a = a_conn.getDB("foo");
    var b = b_conn.getDB("foo");

    /* force the oplog to roll */
    if (new Date() % 2 == 0) {
        print("ROLLING OPLOG AS PART OF TEST (we only do this sometimes)");
        var pass = 1;
        var first = a.getSisterDB("local").oplog.rs.find().sort({ $natural: 1 }).limit(1)[0];
        a.roll.insert({ x: 1 });
        while (1) {
            for (var i = 0; i < 10000; i++)
                a.roll.update({}, { $inc: { x: 1} });
            var op = a.getSisterDB("local").oplog.rs.find().sort({ $natural: 1 }).limit(1)[0];
            if (tojson(op.h) != tojson(first.h)) {
                printjson(op);
                printjson(first);
                break;
            }
            pass++;
            a.getLastError(2); // unlikely secondary isn't keeping up, but let's avoid possible intermittent issues with that.
        }
        print("PASSES FOR OPLOG ROLL: " + pass);
    }
    else {
        print("NO ROLL");
    }

    a.bar.insert({ q: 1, a: "foo" });
    a.bar.insert({ q: 2, a: "foo", x: 1 });
    a.bar.insert({ q: 3, bb: 9, a: "foo" });

    assert(a.bar.count() == 3, "t.count");

    // wait for secondary to get this data
    wait(function () { return ifReady(b, function() { return b.bar.count() == 3 }); });

    A.runCommand({ replSetTest: 1, blind: true });
    reconnect(a,b);
    wait(function () { return B.isMaster().ismaster; });

    b.bar.insert({ q: 4 });
    b.bar.insert({ q: 5 });
    b.bar.insert({ q: 6 });
    assert(b.bar.count() == 6, "u.count");

    // a should not have the new data as it was in blind state.
    B.runCommand({ replSetTest: 1, blind: true });
    print("*************** wait for server to reconnect ****************");
    reconnect(a,b);
    A.runCommand({ replSetTest: 1, blind: false });
    reconnect(a,b);

    print("*************** B ****************");
    wait(function () { try { return !B.isMaster().ismaster; } catch(e) { return false; } });
    print("*************** A ****************");
    reconnect(a,b); 
    wait(function () {
        try {
          return A.isMaster().ismaster;
        } catch(e) {
          return false;
        }
      });

    assert(a.bar.count() == 3, "t is 3");
    a.bar.insert({ q: 7 });
    a.bar.insert({ q: 8 });
    {
        assert(a.bar.count() == 5);
        var x = a.bar.find().toArray();
        assert(x[0].q == 1, '1');
        assert(x[1].q == 2, '2');
        assert(x[2].q == 3, '3');
        assert(x[3].q == 7, '7');
        assert(x[4].q == 8, '8');
    }

    // A is 1 2 3 7 8
    // B is 1 2 3 4 5 6

    // bring B back online  
    B.runCommand({ replSetTest: 1, blind: false });
    reconnect(a,b);

    wait(function () { return B.isMaster().ismaster || B.isMaster().secondary; });

    // everyone is up here...
    assert(A.isMaster().ismaster || A.isMaster().secondary, "A up");
    assert(B.isMaster().ismaster || B.isMaster().secondary, "B up");
    replTest.awaitReplication();

    friendlyEqual(a.bar.find().sort({ _id: 1 }).toArray(), b.bar.find().sort({ _id: 1 }).toArray(), "server data sets do not match");

    pause("rollback.js SUCCESS");
    replTest.stopSet(signal);
};


var reconnect = function(a,b) {
  wait(function() { 
      try {
        a.bar.stats();
        b.bar.stats();
        return true;
      } catch(e) {
        print(e);
        return false;
      }
    });
};

print("rollback.js");
doTest( 15 );
