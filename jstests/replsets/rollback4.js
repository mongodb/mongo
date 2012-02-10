//Test for SERVER-3650 (rollback from slave)
if (0) { // enable for SERVER-3772

var num = 7;
var host = getHostName();
var name = "rollback4";

var replTest = new ReplSetTest( {name: name, nodes: num} );
var config = replTest.getReplSetConfig();

// set preferred masters
config.members[0].priority = 3
config.members[6].priority = 2
// all other are 1

var nodes = replTest.startSet();
replTest.initiate(config);
replTest.awaitReplication()
replTest.bridge();

replTest.waitForMaster();
var master = replTest.getMaster();
printjson(master.adminCommand("replSetGetStatus"));

var mColl = master.getCollection('test.foo');

mColl.insert({});
printjson(master.adminCommand("replSetGetStatus"));
printjson(master.adminCommand({getLastError:1, w:7, wtimeout:30*1000}));

// partition 012 | 3456 with 0 and 6 the old and new master


printjson({startPartition: new Date()});
replTest.partition(0,3)
replTest.partition(0,4)
replTest.partition(0,5)
replTest.partition(0,6)
replTest.partition(1,3)
replTest.partition(1,4)
replTest.partition(1,5)
replTest.partition(1,6)
replTest.partition(2,3)
replTest.partition(2,4)
replTest.partition(2,5)
replTest.partition(2,6)
printjson({endPartition: new Date()});

var gotThrough = 0
try {
    while (true){
        mColl.insert({})
        out = master.adminCommand({getLastError:1, w:3});
        if (out.err)
            break;

        gotThrough++;
    }
}
catch (e) {
    print("caught exception");
}

printjson({gotThrough: gotThrough});
printjson({cantWriteOldPrimary: new Date()});
printjson(master.adminCommand("replSetGetStatus"));

assert(gotThrough > 0, "gotOneThrough");

sleep(5*1000); // make sure new seconds field in opTime

replTest.waitForMaster();
var master2 = replTest.getMaster();
printjson(master2.adminCommand("replSetGetStatus"));

var m2Coll = master2.getCollection('test.foo');

var sentinel = {_id: 'sentinel'} // used to detect which master's data is used
m2Coll.insert(sentinel);
printjson(master2.adminCommand({getLastError:1, w:4, wtimeout:30*1000}));
printjson(master2.adminCommand("replSetGetStatus"));

m2Coll.insert({}); // this shouldn't be necessary but the next GLE doesn't work without it

printjson({startUnPartition: new Date()});
replTest.unPartition(0,3)
replTest.unPartition(0,4)
replTest.unPartition(0,5)
replTest.unPartition(0,6)
replTest.unPartition(1,3)
replTest.unPartition(1,4)
replTest.unPartition(1,5)
replTest.unPartition(1,6)
replTest.unPartition(2,3)
replTest.unPartition(2,4)
replTest.unPartition(2,5)
replTest.unPartition(2,6)
printjson({endUnPartition: new Date()});

assert.soon(function() {
    try {
        printjson(master2.adminCommand({getLastError:1, w:7, wtimeout:30*1000}));
        return true;
    }
    catch (e) {
        print("getLastError returned an exception; retrying");
        print(e);
        return false;
    }
});

printjson(master2.adminCommand("replSetGetStatus"));

assert.soon(function() {return master.adminCommand('isMaster').ismaster},
            "Node 0 back to primary",
            60*1000/*needs to be longer than LeaseTime*/);
printjson(master.adminCommand("replSetGetStatus"));

// make sure old master rolled back to new master
assert.eq(m2Coll.count(sentinel), 1, "check sentinal on node 6");
assert.eq(mColl.count(sentinel), 1, "check sentinal on node 0");

replTest.stopSet();

}


