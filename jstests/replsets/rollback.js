// try running as :
// 
//   mongo --nodb rollback.js | tee out | grep -v ^m31
//

var debugging = 0;

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
        print("waiting " + w);
        if( ++n == 2 )
            print("" + f); 
        sleep(2000);
    }
//    print("done waiting " + w);
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
    a = conns[0];
    A = a.getDB("admin");
    b = conns[1];
    a.setSlaveOk();
    b.setSlaveOk();
    B = b.getDB("admin");
    assert(master == conns[0], "conns[0] assumed to be master");
    assert(a == master);

    //deb(master);

    // Make sure we have an arbiter
    assert.soon(function () {
        res = conns[2].getDB("admin").runCommand({ replSetGetStatus: 1 });
        //printjson(res);
        return res.myState == 7;
    }, "Arbiter failed to initialize.");

    // Wait for initial replication
    var t = a.getDB("foo").bar;
    var u = b.getDB("foo").bar;
    t.insert({ q: 1, a: "foo" });
    t.insert({ q: 2, a: "foo", x: 1 });
    t.insert({ q: 3, bb: 9, a: "foo" });

    assert(t.count() == 3, "t.count");

    // wait for secondary to get this data
    wait(function () { return u.count() == 3; });

    A.runCommand({ replSetTest: 1, blind: true });
    wait(function () { return B.isMaster().ismaster; });
    //print("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    //printjson(B.isMaster());
    u.insert({ q: 4 });
    u.insert({ q: 5 });
    u.insert({ q: 6 });
    assert(u.count() == 6, "u.count");

    // a should not have the new data as it was in blind state.
    B.runCommand({ replSetTest: 1, blind: true });
    A.runCommand({ replSetTest: 1, blind: false });
    wait(function () { return !B.isMaster().ismaster; });
    wait(function () { return A.isMaster().ismaster; });

    printjson(A.isMaster());
    assert(t.count() == 3, "t is 3");
    t.insert({ q: 7 });
    t.insert({ q: 8 });
    {
        assert(t.count() == 5);
        var x = t.find().toArray();
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

    wait(function () { return B.isMaster().ismaster || B.isMaster().secondary; });

    // everyone is up here...
    assert(A.isMaster().ismaster || A.isMaster().secondary, "A up");
    assert(B.isMaster().ismaster || B.isMaster().secondary, "B up");

    friendlyEqual(t.find().sort({ _id: 1 }).toArray(), u.find().sort({ _id: 1 }).toArray(), "server data sets do not match");

    pause("SUCCESS");
    replTest.stopSet(signal);
}

doTest( 15 );
