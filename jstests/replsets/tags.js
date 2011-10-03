
var num = 5;
var host = getHostName();
var name = "tags";

var replTest = new ReplSetTest( {name: name, nodes: num, startPort:31000} );
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
                "important" : {"dc" : 2, "server" : 3},
                "a machine" : {"server" : 1}
            }
        }});

var master = replTest.getMaster();

var config = master.getDB("local").system.replset.findOne();

printjson(config);
var modes = config.settings.getLastErrorModes;
assert.eq(typeof modes, "object");
assert.eq(modes.important.dc, 2);
assert.eq(modes.important.server, 3);
assert.eq(modes["a machine"]["server"], 1);

config.version++;
config.members[1].priority = 1.5;
config.members[2].priority = 2;
modes.rack = {"sf" : 1};
modes.niceRack = {"sf" : 2};
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
assert.eq(modes.important.server, 3);
assert.eq(modes["a machine"]["server"], 1);
assert.eq(modes.rack["sf"], 1);
assert.eq(modes.niceRack["sf"], 2);

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
print("2 should be primary");
master = replTest.getMaster();

printjson(master.getDB("admin").runCommand({replSetGetStatus:1}));

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
assert.eq(result.code, 14830);
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

replTest.partition(2, 0);
replTest.partition(2, 1);
replTest.stop(2);

print("1 must become primary here because otherwise the other members will take too long timing out their old sync threads");
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

