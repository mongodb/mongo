/**
 * start a slave with a long delay (1 hour) and do some writes while it is initializing. Make sure it
 * syncs all of these writes before going into syncDelay.
 */
load("jstests/replsets/rslib.js");

var name = "slaveDelay2";
var host = getHostName();


var initialize = function() {
    var replTest = new ReplSetTest( {name: name, nodes: 1} );

    var nodes = replTest.startSet();

    replTest.initiate();

    var master = replTest.getMaster().getDB(name);

    waitForAllMembers(master);

    return replTest;
};

var populate = function(master) {
    // insert records
    var bulk = master.foo.initializeUnorderedBulkOp();
    for (var i =0; i<1000; i++) {
        bulk.insert({ _id: i });
    }

    assert.writeOK(bulk.execute());
}

doTest = function( signal ) {
    var replTest = initialize();
    var master = replTest.getMaster().getDB(name);
    populate(master);
    var admin = master.getSisterDB("admin");

    var conn = MongoRunner.runMongod({port : 31008, dbpath : name + "-sd", useHostname: true,
                                      replSet: name, oplogSize : 128});
    conn.setSlaveOk();

    config = master.getSisterDB("local").system.replset.findOne();
    config.version++;
    config.members.push({_id : 1, host : host+":31008",priority:0, slaveDelay:3600});
    var ok = admin.runCommand({replSetReconfig : config});
    assert.eq(ok.ok,1);

    // do inserts during initial sync
    for (var i = 0; i<100; i++) {
        master.foo.insert({x:i});
    }

    //check if initial sync is done
    var state = master.getSisterDB("admin").runCommand({replSetGetStatus:1});
    printjson(state);
    if (state.members[1].state == 2) {
        print("NOTHING TO CHECK");
        replTest.stopSet();
        return;
    }

    // if we're here, then 100 docs were inserted before initial sync completed
    waitForAllMembers(master, 3 * 60 * 1000);

    // initial sync has completed. ensure slaveDelay did not delay the existence of the docs
    for (var i=0; i<100; i++) {
        var obj = conn.getDB(name).foo.findOne({x : i});
        assert(obj);
    }

    replTest.stopSet();
}

doTest(15);
