// a test of rollback in replica sets
//
// try running as :
// 
//   mongo --nodb rollback2.js | tee out | grep -v ^m31
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
        if (n % 4 == 0)
            print("rollback2.js waiting " + w);
        if (++n == 4) {
            print("" + f);
        }
        assert(n < 200, 'tried 200 times, giving up');
        sleep(1000);
    }
}

function dbs_match(a, b) {
    print("dbs_match");

    var ac = a.system.namespaces.find().sort({name:1}).toArray();
    var bc = b.system.namespaces.find().sort({name:1}).toArray();
    if (!friendlyEqual(ac, bc)) {
        print("dbs_match: namespaces don't match");
        print("\n\n");
        printjson(ac);
        print("\n\n");
        printjson(bc);
        print("\n\n");
        return false;
    }

    var c = a.getCollectionNames();
    for( var i in c ) {
        print("checking " + c[i]);
        if( !friendlyEqual( a[c[i]].find().sort({_id:1}).toArray(), b[c[i]].find().sort({_id:1}).toArray() ) ) { 
            print("dbs_match: collections don't match " + c[i]);
            return false;
        }
    }
    return true;
}

/* these writes will be initial data and replicate everywhere. */
function doInitialWrites(db) {
    t = db.bar;
    t.insert({ q:0});
    t.insert({ q: 1, a: "foo" });
    t.insert({ q: 2, a: "foo", x: 1 });
    t.insert({ q: 3, bb: 9, a: "foo" });
    t.insert({ q: 40, a: 1 });
    t.insert({ q: 40, a: 2 });
    t.insert({ q: 70, txt: 'willremove' });

    db.createCollection("kap", { capped: true, size: 5000 });
    db.kap.insert({ foo: 1 })

    // going back to empty on capped is a special case and must be tested
    db.createCollection("kap2", { capped: true, size: 5501 });
}

/* these writes on one primary only and will be rolled back. */
function doItemsToRollBack(db) {
    t = db.bar;
    t.insert({ q: 4 });
    t.update({ q: 3 }, { q: 3, rb: true });

    t.remove({ q: 40 }); // multi remove test

    t.update({ q: 2 }, { q: 39, rb: true });

    // rolling back a delete will involve reinserting the item(s)
    t.remove({ q: 1 });

    t.update({ q: 0 }, { $inc: { y: 1} });

    db.kap.insert({ foo: 2 })
    db.kap2.insert({ foo: 2 })

    // create a collection (need to roll back the whole thing)
    db.newcoll.insert({ a: true });

    // create a new empty collection (need to roll back the whole thing)
    db.createCollection("abc");
}

function doWritesToKeep2(db) {
    t = db.bar;
    t.insert({ txt: 'foo' });
    t.remove({ q: 70 });
    t.update({ q: 0 }, { $inc: { y: 33} });
}

function verify(db) {
    print("verify");
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
    wait(function () {
        var status = A.runCommand({replSetGetStatus : 1});
        return status.members[1].state == 2;
      });

    doInitialWrites(a);

    // wait for secondary to get this data
    wait(function () { return ifReady(a, function() { return ifReady(b, function() { return b.bar.count() == a.bar.count(); }); }); });
    wait(function () {
        var status = A.runCommand({replSetGetStatus : 1});
        return status.members[1].state == 2;
      });

    
    A.runCommand({ replSetTest: 1, blind: true });
    reconnect(a, b);
    
    wait(function () { return B.isMaster().ismaster; });

    doItemsToRollBack(b);

    // a should not have the new data as it was in blind state.
    B.runCommand({ replSetTest: 1, blind: true });
    reconnect(a, b);
    A.runCommand({ replSetTest: 1, blind: false });
    reconnect(a,b);

    wait(function () { try { return !B.isMaster().ismaster; } catch(e) { return false; } });
    wait(function () { try { return A.isMaster().ismaster; } catch(e) { return false; } });

    assert(a.bar.count() >= 1, "count check");
    doWritesToKeep2(a);

    // A is 1 2 3 7 8
    // B is 1 2 3 4 5 6

    // bring B back online
    // as A is primary, B will roll back and then catch up
    B.runCommand({ replSetTest: 1, blind: false });
    reconnect(a,b);
    
    wait(function () { return B.isMaster().ismaster || B.isMaster().secondary; });
    
    // everyone is up here...
    replTest.awaitReplication();

    // theoretically, a read could slip in between StateBox::change() printing
    // replSet SECONDARY
    // and the replset actually becoming secondary
    // so we're trying to wait for that here
    print("waiting for secondary");
    assert.soon(function() {
        try {
          var aim = A.isMaster();
          var bim = B.isMaster();
          return (aim.ismaster || aim.secondary) &&
            (bim.ismaster || bim.secondary);
        }
        catch(e) {
          print("checking A and B: "+e);
        }
      });
    
    verify(a);

    assert( dbs_match(a,b), "server data sets do not match after rollback, something is wrong");

    pause("rollback2.js SUCCESS");
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

print("rollback2.js");

doTest( 15 );
