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
        if (n % 4 == 0)
            print("rollback3.js waiting " + w);
        if (++n == 4) {
            print("" + f);
        }
        if (n == 200) {
            print("rollback3.js failing waited too long");
            throw "wait error";
        }
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
        // system.indexes doesn't have _id so the more involved sort here: 
        if (!friendlyEqual(a[c[i]].find().sort({ _id: 1, ns:1, name:1 }).toArray(), b[c[i]].find().sort({ _id: 1, ns:1,name:1 }).toArray())) {
            print("dbs_match: collections don't match " + c[i]);
            if (a[c[i]].count() < 12) {
                printjson(a[c[i]].find().sort({ _id: 1 }).toArray());
                printjson(b[c[i]].find().sort({ _id: 1 }).toArray());
            }
            return false;
        }
    }
    return true;
}

/* these writes will be initial data and replicate everywhere. */
function doInitialWrites(db) {
    db.b.insert({ x: 1 });
    db.b.ensureIndex({ x: 1 });
    db.oldname.insert({ y: 1 });
    db.oldname.insert({ y: 2 });
    db.oldname.ensureIndex({ y: 1 },true);
    t = db.bar;
    t.insert({ q:0});
    t.insert({ q: 1, a: "foo" });
    t.insert({ q: 2, a: "foo", x: 1 });
    t.insert({ q: 3, bb: 9, a: "foo" });
    t.insert({ q: 40333333, a: 1 });
    for (var i = 0; i < 200; i++) t.insert({ i: i });
    t.insert({ q: 40, a: 2 });
    t.insert({ q: 70, txt: 'willremove' });

    db.createCollection("kap", { capped: true, size: 5000 });
    db.kap.insert({ foo: 1 })
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

    // drop a collection - we'll need all its data back!
    t.drop();

    // drop an index - verify it comes back
    db.b.dropIndexes();

    // two to see if we transitively rollback?
    db.oldname.renameCollection("newname");
    db.newname.renameCollection("fooname");

    assert(db.fooname.count() > 0, "count rename");

    // test roll back (drop) a whole database   
    abc = db.getSisterDB("abc");
    abc.foo.insert({ x: 1 });
    abc.bar.insert({ y: 999 });

    // test making and dropping a database
    //mkd = db.getSisterDB("mkd");
    //mkd.c.insert({ y: 99 });
    //mkd.dropDatabase();
}

function doWritesToKeep2(db) {
    t = db.bar;
    t.insert({ txt: 'foo' });
    t.remove({ q: 70 });
    t.update({ q: 0 }, { $inc: { y: 33} });
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

    // wait for secondary to get this data
    wait(function () { return ifReady(a, function() { return ifReady(b, function() { return b.bar.count() == a.bar.count(); }); }); });

    A.runCommand({ replSetTest: 1, blind: true });
    reconnect(a,b);
    wait(function () { try { return B.isMaster().ismaster; } catch(e) { return false; } });

    doItemsToRollBack(b);

    // a should not have the new data as it was in blind state.
    B.runCommand({ replSetTest: 1, blind: true });
    reconnect(a,b);

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
    assert(A.isMaster().ismaster || A.isMaster().secondary, "A up");
    assert(B.isMaster().ismaster || B.isMaster().secondary, "B up");
    replTest.awaitReplication();
    
    assert( dbs_match(a,b), "server data sets do not match after rollback, something is wrong");

    pause("rollback3.js SUCCESS");
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

print("rollback3.js");
doTest( 15 );
