
var num = 5;
var host = getHostName();
var name = "tags";

var replTest = new ReplSetTest( {name: name, nodes: num, startPort:31000} );
var nodes = replTest.startSet();
var port = replTest.ports;
replTest.initiate({_id : name, members :
        [
         {_id:0, host : host+":"+port[0], tags : ["0", "dc.ny.rk1", "machine"]},
         {_id:1, host : host+":"+port[1], tags : ["1", "dc.ny.rk1", "machine"]},
         {_id:2, host : host+":"+port[2], tags : ["2", "dc.ny.rk2", "machine"]},
         {_id:3, host : host+":"+port[3], tags : ["3", "dc.sf.rk1", "machine"]},
         {_id:4, host : host+":"+port[4], tags : ["4", "dc.sf.rk2", "machine"]},
        ],
        settings : {
            getLastErrorModes : {
                "important" : {"dc" : 2, "machine" : 3},
                "a machine" : {"machine" : 1}
            }
        }});

var master = replTest.getMaster();

var config = master.getDB("local").system.replset.findOne();

printjson(config);
var modes = config.settings.getLastErrorModes;
assert.eq(typeof modes, "object");
assert.eq(modes.important.dc, 2);
assert.eq(modes.important.machine, 3);
assert.eq(modes["a machine"]["machine"], 1);

config.version++;
config.members[2].priority = 2;
modes.rack = {"dc.sf" : 1};
modes.niceRack = {"dc.sf" : 2};
modes["a machine"]["2"] = 1;
modes.on2 = {"2" : 1}

try {
    master.getDB("admin").runCommand({replSetReconfig : config});
}
catch(e) {
    print(e);
}

replTest.awaitReplication();

print("primary should now be 2");
master = replTest.getMaster();
config = master.getDB("local").system.replset.findOne();
printjson(config);

modes = config.settings.getLastErrorModes;
assert.eq(typeof modes, "object");
assert.eq(modes.important.dc, 2);
assert.eq(modes.important.machine, 3);
assert.eq(modes["a machine"]["machine"], 1);
assert.eq(modes.rack["dc.sf"], 1);
assert.eq(modes.niceRack["dc.sf"], 2);

print("bridging");
replTest.bridge();

replTest.partition(0, 3);
replTest.partition(0, 4);
replTest.partition(1, 3);
replTest.partition(1, 4);
replTest.partition(2, 3);
replTest.partition(2, 4);
replTest.partition(3, 4);
print("done bridging");

print("test1");
master = replTest.getMaster();

var timeout = 20000;

master.getDB("foo").bar.insert({x:1});
var result = master.getDB("foo").runCommand({getLastError:1,w:"rack",wtimeout:timeout});
printjson(result);
assert.eq(result.err, "timeout");

replTest.unPartition(1,4);

print("test2");
master.getDB("foo").bar.insert({x:1});
result = master.getDB("foo").runCommand({getLastError:1,w:"rack",wtimeout:timeout});
printjson(result);
assert.eq(result.err, null);

print("test3");
result = master.getDB("foo").runCommand({getLastError:1,w:"niceRack",wtimeout:timeout});
printjson(result);
assert.eq(result.err, "timeout");

replTest.unPartition(3,4);

print("test4");
result = master.getDB("foo").runCommand({getLastError:1,w:"niceRack",wtimeout:timeout});
printjson(result);
assert.eq(result.err, null);

print("non-existent w");
result = master.getDB("foo").runCommand({getLastError:1,w:"blahblah",wtimeout:timeout});
printjson(result);
assert.eq(result.assertionCode, 14830);
assert.eq(result.ok, 0);

print("test on2");
master.getDB("foo").bar.insert({x:1});
result = master.getDB("foo").runCommand({getLastError:1,w:"on2",wtimeout:0});
printjson(result);
assert.eq(result.err, null);

print("test two on the primary");
master.getDB("foo").bar.insert({x:1});
result = master.getDB("foo").runCommand({getLastError:1,w:"a machine",wtimeout:0});
printjson(result);
assert.eq(result.err, null);

print("test5");
master.getDB("foo").bar.insert({x:1});
result = master.getDB("foo").runCommand({getLastError:1,w:"important",wtimeout:timeout});
printjson(result);
assert.eq(result.err, null);

replTest.unPartition(1,3);

replTest.stop(2);

master = replTest.getMaster();

print("test6");
master.getDB("foo").bar.insert({x:1});
result = master.getDB("foo").runCommand({getLastError:1,w:"niceRack",wtimeout:timeout});
printjson(result);
assert.eq(result.err, null);

print("test on2");
master.getDB("foo").bar.insert({x:1});
result = master.getDB("foo").runCommand({getLastError:1,w:"on2",wtimeout:timeout});
printjson(result);
assert.eq(result.err, "timeout");

