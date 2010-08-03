// test rollback in replica sets

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
        if( n % 4 == 0 )
            print("waiting " + w);
        if (++n == 4) {
            print("" + f);
        }
        sleep(1000);
    }
}

/* these writes will be initial data and replicate everywhere. */
function doInitialWrites(db) {
    t = db.bar;
    t.insert({ q: 1, a: "foo" });
    t.insert({ q: 2, a: "foo", x: 1 });
    t.insert({ q: 3, bb: 9, a: "foo" });
}

/* these writes on one primary only and will be rolled back. */
function doItemsToRollBack(db) {
    t = db.bar;
    t.insert({ q: 4 });
    t.update({ q: 3 }, { q: 3, rb: true });
    t.update({ q: 2 }, { q: 39, rb: true });

    // rolling back a delete will involve reinserting the item(s)
    t.remove({q:1});
}

function doWritesToKeep2(db) {
    t = db.bar;
    t.insert({ txt: 'foo' });
}

function verify(db) {
    t = db.bar;
    assert(t.find({ q: 1 }).count() == 1);
    assert(t.find({ txt: 'foo' }).count() == 1);
    assert(t.find({ q: 4 }).count() == 0);
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
    doInitialWrites(a);
    assert(a.bar.count() == 3, "t.count");

    // wait for secondary to get this data
    wait(function () { return b.bar.count() == 3; });

    A.runCommand({ replSetTest: 1, blind: true });
    wait(function () { return B.isMaster().ismaster; });

    doItemsToRollBack(b);

    // a should not have the new data as it was in blind state.
    B.runCommand({ replSetTest: 1, blind: true });
    A.runCommand({ replSetTest: 1, blind: false });
    wait(function () { return !B.isMaster().ismaster; });
    wait(function () { return A.isMaster().ismaster; });

    assert(a.bar.count() == 3, "t is 3");
    doWritesToKeep2(a);
    a.bar.insert({ q: 8, z: 99 });

    // A is 1 2 3 7 8
    // B is 1 2 3 4 5 6

    // bring B back online
    // as A is primary, B will roll back and then catch up
    B.runCommand({ replSetTest: 1, blind: false });

    wait(function () { return B.isMaster().ismaster || B.isMaster().secondary; });

    // everyone is up here...
    assert(A.isMaster().ismaster || A.isMaster().secondary, "A up");
    assert(B.isMaster().ismaster || B.isMaster().secondary, "B up");

    assert(a.bar.findOne({ q: 8 }).z == 99);
    verify(a);

    friendlyEqual(a.bar.find().sort({ _id: 1 }).toArray(), b.bar.find().sort({ _id: 1 }).toArray(), "server data sets do not match");

    pause("SUCCESS");
    replTest.stopSet(signal);
}

doTest( 15 );
