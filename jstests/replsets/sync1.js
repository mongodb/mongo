// FAILING TEST
// replication is not rolled back

doTest = function( signal ) {

    var replTest = new ReplSetTest( {name: 'testSet', nodes: 3} );
    var nodes = replTest.startSet({oplogSize : "40"});

    sleep(5000);

    replTest.initiate();

    // get master
    var master = replTest.getMaster();
    var dbs = [master.getDB("foo")];

    for (var i in nodes) {
        if (nodes[i]+"" == master+"") {
            continue;
        }
        dbs.push(nodes[i].getDB("foo"));
        nodes[i].setSlaveOk();
    }

    dbs[0].bar.drop();

    // slow things down a bit
    dbs[0].bar.ensureIndex({x:1});
    dbs[0].bar.ensureIndex({y:1});
    dbs[0].bar.ensureIndex({z:1});
    dbs[0].bar.ensureIndex({w:1});
    
    var ok = false;
    var inserts = 100000;

    for (var i=0; i<inserts; i++) {
        dbs[0].bar.insert({x:"foo"+i, y:"bar"+i, z:i, w:"biz baz bar boo"});
    }

    dbs[0].getSisterDB("admin").runCommand({replSetTest:1, blind : true});

    // yay! there are out-of-date nodes
    var max1 = dbs[1].bar.find().sort({z:-1}).limit(1).next();
    var max2 = dbs[2].bar.find().sort({z:-1}).limit(1).next();

    if (max1.z == inserts && max2.z == inserts) {
        print ("try increasing # if inserts and running again");
        replTest.stopSet( signal );
        return;
    }

    // wait for a new master to be elected
    sleep(5000);

    // figure out who is master now
    var newMaster = replTest.getMaster();

    print("**********************************************");
    assert(newMaster+"" != master+"", "new master is "+newMaster+", old master was "+master);
    print("new master is "+newMaster+", old master was "+master);

    var count = 0;
    do {
        max1 = dbs[1].bar.find().sort({z:-1}).limit(1).next();
        max2 = dbs[2].bar.find().sort({z:-1}).limit(1).next();

	print(count+" sync1.js: WAITING FOR MATCH " + Date() + " z[1]:" + max1.z + " z[2]:" + max2.z);

	//        printjson(max1);
	//        printjson(max2);

        sleep(2000);

        count++;
        if (count == 100) {
            assert(false, "replsets/sync1.js fails timing out");
            replTest.stopSet( signal );
            return;
        }
    } while (max1.z != max2.z);

    // okay, now they're caught up.  We have a max:
    var max = max1.z;

    // now, let's see if rollback works
    var result = dbs[0].getSisterDB("admin").runCommand({replSetTest : 1, blind : false});
    dbs[0].getMongo().setSlaveOk();

    printjson(result);
    sleep(5000);

    // FAIL! This never resyncs
    // now this should resync
    var max0;
    count = 0;
    do {
        max0 = dbs[0].bar.find().sort({z:-1}).limit(1).next();

	print(count+" sync1.js: WAITING FOR MATCH " + Date() + " z[0]:" + max0.z + " z:" + max);

        sleep(2000);

        count++;
        if (count == 100) {
            assert(false, "replsets/sync1.js fails timing out");
            replTest.stopSet( signal );
            return;
        }
    } while (max0.z != max);

    replTest.stopSet( signal );
}

//doTest( 15 );
