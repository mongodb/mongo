// test rollback of replica sets

var debugging=0;

w = 0;

function pause(s) {
    // for debugging just to keep processes running
    print("\nsync1.js: " + s);
    if (debugging) {
        while (1) {
            print("\nsync1.js: " + s);
            sleep(4000);
        }
    }
}

doTest = function (signal) {

    var replTest = new ReplSetTest({ name: 'testSet', nodes: 3 });
    var nodes = replTest.startSet({ oplogSize: "40" });

    sleep(5000);

    print("\nsync1.js ********************************************************************** part 0");
    replTest.initiate();

    // get master
    print("\nsync1.js ********************************************************************** part 1");
    var master = replTest.getMaster();
    print("\nsync1.js ********************************************************************** part 2");
    var dbs = [master.getDB("foo")];

    for (var i in nodes) {
        if (nodes[i] + "" == master + "") {
            continue;
        }
        dbs.push(nodes[i].getDB("foo"));
        nodes[i].setSlaveOk();
    }

    print("\nsync1.js ********************************************************************** part 3");
    dbs[0].bar.drop();

    print("\nsync1.js ********************************************************************** part 4");
    // slow things down a bit
    dbs[0].bar.ensureIndex({ x: 1 });
    dbs[0].bar.ensureIndex({ y: 1 });
    dbs[0].bar.ensureIndex({ z: 1 });
    dbs[0].bar.ensureIndex({ w: 1 });

    var ok = false;
    var inserts = 100000;

    print("\nsync1.js ********************************************************************** part 5");

    for (var i = 0; i < inserts; i++) {
        dbs[0].bar.insert({ x: "foo" + i, y: "bar" + i, z: i, w: "biz baz bar boo" });
    }

    var status;
    do {
        sleep(1000);
        status = dbs[0].getSisterDB("admin").runCommand({ replSetGetStatus: 1 });
    } while (status.members[1].state != 2 && status.members[2].state != 2);

    print("\nsync1.js ********************************************************************** part 6");
    dbs[0].getSisterDB("admin").runCommand({ replSetTest: 1, blind: true });

    print("\nsync1.js ********************************************************************** part 7");

    sleep(5000);

    var max1;
    var max2;
    var count = 0;
    while (1) {
        try {
            max1 = dbs[1].bar.find().sort({ z: -1 }).limit(1).next();
            max2 = dbs[2].bar.find().sort({ z: -1 }).limit(1).next();
        }
        catch (e) {
            print("\nsync1.js couldn't get max1/max2; retrying " + e);
            sleep(2000);
            count++;
            if (count == 50) {
                assert(false, "errored out 50 times");
            }
            continue;
        }
        break;
    }

    // wait for a new master to be elected
    sleep(5000);
    var newMaster;

    print("\nsync1.js ********************************************************************** part 9");

    for (var q = 0; q < 10; q++) {
        // figure out who is master now
        newMaster = replTest.getMaster();
        if (newMaster + "" != master + "")
            break;
        sleep(2000);
        if (q > 6) print("sync1.js zzz....");
    }

    assert(newMaster + "" != master + "", "new master is " + newMaster + ", old master was " + master);

    print("\nsync1.js new master is " + newMaster + ", old master was " + master);

    print("\nsync1.js ********************************************************************** part 9.1");

    count = 0;
    countExceptions = 0;
    do {
        try {
            max1 = dbs[1].bar.find().sort({ z: -1 }).limit(1).next();
            max2 = dbs[2].bar.find().sort({ z: -1 }).limit(1).next();
        }
        catch (e) {
            if ( countExceptions++ > 500 ){
                assert(false ,"too many exceptions, failing" );
            }
            print("\nsync1.js: exception querying; will sleep and try again " + e);
            sleep(2000);
            continue;
        }

        print("\nsync1.js waiting for match " + count + " " + Date() + " z[1]:" + max1.z + " z[2]:" + max2.z);

        //        printjson(max1);
        //        printjson(max2);

        sleep(2000);

        count++;
        if (count == 100) {
            pause("fail phase 1");
            assert(false, "replsets/\nsync1.js fails timing out");
            replTest.stopSet(signal);
            return;
        }
    } while (max1.z != max2.z);

    // okay, now they're caught up.  We have a max: max1.z

    print("\nsync1.js ********************************************************************** part 10");

    // now, let's see if rollback works
    var result = dbs[0].getSisterDB("admin").runCommand({ replSetTest: 1, blind: false });
    dbs[0].getMongo().setSlaveOk();

    printjson(result);
    sleep(5000);

    // FAIL! This never resyncs
    // now this should resync
    print("\nsync1.js ********************************************************************** part 11");
    var max0 = null;
    count = 0;
    do {
        try {
            max0 = dbs[0].bar.find().sort({ z: -1 }).limit(1).next();
        }
        catch (e) {
            print("\nsync1.js part 11 exception on bar.find() will sleep and try again " + e);
            sleep(2000);
            continue;
        }

        printjson(max1);
        printjson(max0);
        print("\nsync1.js part 11 waiting for match " + count + " " + Date() + " z[0]:" + max0.z + " z:" + max1.z);

        sleep(2000);

        count++;
        if (count == 100) {
            pause("fail part 11");
            assert(false, "replsets/\nsync1.js fails timing out");
            replTest.stopSet(signal);
            return;
        }
        print("||||| count:" + count);
        printjson(max0);
    } while (!max0 || max0.z != max1.z);

    print("\nsync1.js ********************************************************************** part 12");
    pause("\nsync1.js success");
    replTest.stopSet(signal);
}

if( 1 || debugging ) {
    doTest( 15 );
}
