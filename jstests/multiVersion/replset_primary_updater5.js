/* SERVER-6071 This test (and the other replset_primary_updater tests) check cross-compatibility of
 * sync_source_feedback's updatePosition command and the OplogReader-based method of updating the
 * primary's knowledge of the secondaries' sync progress. This is done through a modified version of
 * the tags.js replicaset js test because tags.js was the test that helped me discover and resolve
 * the largest number of bugs when creating the updatePosition command. In tags.js, a chain forms
 * running from member 4 to member 1 to member 2 (nodes n5, n2, and n3, respectively). Between the
 * six replset_primary_updater tests, we run tags.js with each possible permutation of new and old
 * nodes along this chain.
 */

if (!_isWindows()) {
function myprint( x ) {
    print( "tags output: " + x );
}


load( './jstests/multiVersion/libs/multi_rs.js' )
load( './jstests/libs/test_background_ops.js' )

var oldVersion = "2.4"
var newVersion = "latest"

var nodes = { n1 : { binVersion : oldVersion },
              n2 : { binVersion : oldVersion },
              n3 : { binVersion : newVersion },
              n4 : { binVersion : oldVersion },
              n5 : { binVersion : newVersion } }

// Wait for a primary node...

var num = 5;
var host = getHostName();
var name = "dannentest";

var replTest = new ReplSetTest( {name: name, nodes: nodes, startPort:31000} );
var nodes = replTest.startSet();
var port = replTest.ports;
replTest.initiate({_id : name, members :
        [
            {_id:0, host : host+":"+port[0], tags : {"server" : "0", "dc" : "ny", "ny" : "1", "rack" : "ny.rk1"}},
            {_id:1, host : host+":"+port[1], tags : {"server" : "1", "dc" : "ny", "ny" : "2", "rack" : "ny.rk1"}},
            {_id:2, host : host+":"+port[2], tags : {"server" : "2", "dc" : "ny", "ny" : "3", "rack" : "ny.rk2", "2" : "this"}},
            {_id:3, host : host+":"+port[3], tags : {"server" : "3", "dc" : "sf", "sf" : "1", "rack" : "sf.rk1"}},
            {_id:4, host : host+":"+port[4], tags : {"server" : "4", "dc" : "sf", "sf" : "2", "rack" : "sf.rk2"}},
        ],
        settings : {
            getLastErrorModes : {
                "2 dc and 3 server" : {"dc" : 2, "server" : 3},
                "1 and 2" : {"server" : 1}
            }
        }});

var master = replTest.getMaster();
// make everyone catch up before reconfig
replTest.awaitReplication();

var config = master.getDB("local").system.replset.findOne();

printjson(config);
var modes = config.settings.getLastErrorModes;
assert.eq(typeof modes, "object");
assert.eq(modes["2 dc and 3 server"].dc, 2);
assert.eq(modes["2 dc and 3 server"].server, 3);
assert.eq(modes["1 and 2"]["server"], 1);

config.version++;
config.members[1].priority = 1.5;
config.members[2].priority = 2;
modes["3 or 4"] = {"sf" : 1};
modes["3 and 4"] = {"sf" : 2};
modes["1 and 2"]["2"] = 1;
modes["2"] = {"2" : 1}

try {
    master.getDB("admin").runCommand({replSetReconfig : config});
}
catch(e) {
    myprint(e);
}

assert.soon(function() {
    try {
        return nodes[2].getDB("admin").isMaster().ismaster;
    } catch (x) {
        return false;
    }
}, 'wait for 2 to be primary', 60000);

myprint("primary is now 2");
master = replTest.getMaster();
config = master.getDB("local").system.replset.findOne();
printjson(config);

modes = config.settings.getLastErrorModes;
assert.eq(typeof modes, "object");
assert.eq(modes["2 dc and 3 server"].dc, 2);
assert.eq(modes["2 dc and 3 server"].server, 3);
assert.eq(modes["1 and 2"]["server"], 1);
assert.eq(modes["3 or 4"]["sf"], 1);
assert.eq(modes["3 and 4"]["sf"], 2);

myprint("bridging");
replTest.bridge();
myprint("bridge 1");
replTest.partition(0, 3);
myprint("bridge 2");
replTest.partition(0, 4);
myprint("bridge 3");
replTest.partition(1, 3);
myprint("bridge 4");
replTest.partition(1, 4);
myprint("bridge 5");
replTest.partition(2, 3);
myprint("bridge 6");
replTest.partition(2, 4);
myprint("bridge 7");
replTest.partition(3, 4);
myprint("done bridging");

myprint("paritions: [0-1-2-0] [3] [4]")
myprint("test1");
myprint("2 should be primary");
master = replTest.getMaster();

printjson(master.getDB("admin").runCommand({replSetGetStatus:1}));

var timeout = 20000;

master.getDB("foo").bar.insert({x:1});
var result = master.getDB("foo").runCommand({getLastError:1,w:"3 or 4",wtimeout:timeout});
printjson(result);
assert.eq(result.err, "timeout");

replTest.unPartition(1,4);

myprint("partitions: [1-4] [0-1-2-0] [3]");
myprint("test2");
master.getDB("foo").bar.insert({x:1});
result = master.getDB("foo").runCommand({getLastError:1,w:"3 or 4",wtimeout:timeout});
printjson(result);
assert.eq(result.err, null);

myprint("partitions: [1-4] [0-1-2-0] [3]");
myprint("test3");
result = master.getDB("foo").runCommand({getLastError:1,w:"3 and 4",wtimeout:timeout});
printjson(result);
assert.eq(result.err, "timeout");

replTest.unPartition(3,4);

myprint("partitions: [0-4-3] [0-1-2-0]");
myprint("31004 should sync from 31001 (31026)");
myprint("31003 should sync from 31004 (31024)");
myprint("test4");
result = master.getDB("foo").runCommand({getLastError:1,w:"3 and 4",wtimeout:timeout});
printjson(result);
assert.eq(result.err, null);

myprint("non-existent w");
result = master.getDB("foo").runCommand({getLastError:1,w:"blahblah",wtimeout:timeout});
printjson(result);
assert.eq(result.code, 14830);
assert.eq(result.ok, 0);

myprint("test mode 2");
master.getDB("foo").bar.insert({x:1});
result = master.getDB("foo").runCommand({getLastError:1,w:"2",wtimeout:0});
printjson(result);
assert.eq(result.err, null);

myprint("test two on the primary");
master.getDB("foo").bar.insert({x:1});
result = master.getDB("foo").runCommand({getLastError:1,w:"1 and 2",wtimeout:0});
printjson(result);
assert.eq(result.err, null);

myprint("test5");
master.getDB("foo").bar.insert({x:1});
result = master.getDB("foo").runCommand({getLastError:1,w:"2 dc and 3 server",wtimeout:timeout});
printjson(result);
assert.eq(result.err, null);

replTest.unPartition(1,3);

replTest.partition(2, 0);
replTest.partition(2, 1);
replTest.stop(2);

myprint("1 must become primary here because otherwise the other members will take too long timing out their old sync threads");
master = replTest.getMaster();

myprint("test6");
master.getDB("foo").bar.insert({x:1});
result = master.getDB("foo").runCommand({getLastError:1,w:"3 and 4",wtimeout:timeout});
printjson(result);
assert.eq(result.err, null);

myprint("test mode 2");
master.getDB("foo").bar.insert({x:1});
result = master.getDB("foo").runCommand({getLastError:1,w:"2",wtimeout:timeout});
printjson(result);
assert.eq(result.err, "timeout");

replTest.stopSet();
myprint("\n\ntags.js SUCCESS\n\n");

}
