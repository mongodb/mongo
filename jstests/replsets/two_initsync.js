// test initial sync failing

// try running as :
//
//   mongo --nodb two_initsync.js | tee out | grep -v ^m31
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
    if (debugging) {
        print("\n\n\n" + obj + "\n\n");
    }
}

w = 0;

function wait(f) {
    w++;
    var n = 0;
    while (!f()) {
        if (n % 4 == 0)
            print("twoinitsync waiting " + w);
        if (++n == 4) {
            print("" + f);
        }
        assert(n < 200, 'tried 200 times, giving up');
        sleep(1000);
    }
}

doTest = function(signal) {
    if (!(typeof TestData.setParameters === 'string') ||
        !TestData.setParameters.includes("use3dot2InitialSync=true")) {
        print("Test should only run with 3.2 style initial sync.");
        return;
    }
    var replTest = new ReplSetTest({name: 'testSet', nodes: 0});

    var first = replTest.add();

    // Initiate replica set
    assert.soon(function() {
        var res = first.getDB("admin").runCommand({replSetInitiate: null});
        return res['ok'] == 1;
    });

    // Get status
    assert.soon(function() {
        var result = first.getDB("admin").runCommand({replSetGetStatus: true});
        return result['ok'] == 1;
    });

    var a = replTest.getPrimary().getDB("two");
    for (var i = 0; i < 20000; i++)
        assert.writeOK(a.coll.insert({
            i: i,
            s: "a                                                                       b"
        }));
    assert.eq(20000, a.coll.find().itcount());

    // Start a second node
    var second = replTest.add();

    // Add the second node.
    // This runs the equivalent of rs.add(newNode);
    replTest.reInitiate(60000);

    var b = second.getDB("admin");

    // attempt to interfere with the initial sync
    b._adminCommand({replSetTest: 1, forceInitialSyncFailure: 1});

    //    wait(function () { return a._adminCommand("replSetGetStatus").members.length == 2; });

    wait(function() {
        return b.isMaster().secondary || b.isMaster().ismaster;
    });

    print("b.isMaster:");
    printjson(b.isMaster());

    second.setSlaveOk();

    print("b.isMaster:");
    printjson(b.isMaster());

    wait(function() {
        var c = b.getSisterDB("two").coll.find().itcount();
        print(c);
        return c == 20000;
    });

    print("two_initsync.js SUCCESS");

    replTest.stopSet(signal);
};

print("two_initsync.js");
doTest(15);
