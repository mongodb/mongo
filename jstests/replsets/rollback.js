
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
    while (!f()) {
        print("waiting " + w);
        sleep(2000);
    }
    print("done waiting " + w);
}

doTest = function (signal) {

    var replTest = new ReplSetTest({ name: 'unicomplex', nodes: 3 });
    var nodes = replTest.nodeList();

    print(tojson(nodes));

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
    b.setSlaveOk();
    B = b.getDB("admin");
    assert(master == conns[0], "conns[0] assumed to be master");

    //deb(master);

    // Make sure we have an arbiter
    assert.soon(function () {
        res = conns[2].getDB("admin").runCommand({ replSetGetStatus: 1 });
        printjson(res);
        return res.myState == 7;
    }, "Arbiter failed to initialize.");

    // Wait for initial replication
    var t = master.getDB("foo").bar;
    var u = b.getDB("foo").bar;
    t.insert({ a: "foo" });
    t.insert({ a: "foo", x: 1 });
    t.insert({ y: 9, a: "foo" });

    assert(t.count() == 3, "t.count");

    //wait( function() { 

    wait(function () { return u.count() == 3; });

    A.runCommand({ replSetTest: 1, blind: true });

    wait(function () { return B.isMaster(); });

    print(u.count());
    pause("SUCCESS");

    replTest.stopSet(signal);
}

    doTest( 15 );   
