if (!_isWindows()) {

var testInsert = function() {
    master.getDB("foo").bar.insert({x:1});
    var result = master.getDB("foo").runCommand({getLastError:1, w:"majority", wtimeout:timeout});
    printjson(result);
    return result;
};

var num = 7;
var host = getHostName();
var name = "tags";
var timeout = 60000;

var replTest = new ReplSetTest( {name: name, nodes: num, startPort:31000} );
var nodes = replTest.startSet();
var port = replTest.ports;
var config = {_id : name, members :
        [
         {_id:0, host : host+":"+port[0], priority : 2},
         {_id:1, host : host+":"+port[1], votes : 3},
         {_id:2, host : host+":"+port[2]},
         {_id:3, host : host+":"+port[3], arbiterOnly : true},
         {_id:4, host : host+":"+port[4], arbiterOnly : true},
         {_id:5, host : host+":"+port[5], arbiterOnly : true},
         {_id:6, host : host+":"+port[6], arbiterOnly : true},
        ],
             };
replTest.initiate(config);

replTest.awaitReplication();

var master = replTest.getMaster();

print("try taking down 4 arbiters");
replTest.stop(3);
replTest.stop(4);

replTest.stop(6);
replTest.remove(6);
replTest.stop(5);
replTest.remove(5);

print("should still be able to write to a majority");
var result = testInsert();
assert.eq(result.err, null);
// majority should be primary + 2 secondaries
assert.eq(result.writtenTo.length, 3);
assert.contains(config.members[0], result);
assert.contains(config.members[1], result);
assert.contains(config.members[2], result);

print("start up some of the arbiters again");
replTest.restart(3);
replTest.restart(4);

print("remove 2 of the arbiters");
config.version = 2;
config.members.pop();
config.members.pop();

try {
    master.getDB("admin").runCommand({replSetReconfig : config});
}
catch (e) {
    print("reconfig error: "+e);
}

replTest.awaitReplication();

replTest.bridge();

master = replTest.getMaster();

print("get back in the groove");
testInsert();
replTest.awaitReplication();

print("makes sure majority works");
result = testInsert();
assert.eq(result.err, null);
assert.eq(result.writtenTo.length, 3);
assert.contains(config.members[0], result);
assert.contains(config.members[1], result);
assert.contains(config.members[2], result);

print("setup: 0,1 | 2,3,4");
replTest.partition(0,2);
replTest.partition(0,3);
replTest.partition(0,4);
replTest.partition(1,2);
replTest.partition(1,3);
replTest.partition(1,4);

print("make sure majority doesn't work");
// primary should now be 2
master = replTest.getMaster();
result = testInsert();
assert.eq(result.err, "timeout");
assert.eq(result.writtenTo.length, 2);
assert.contains(config.members[0], result);
assert.contains(config.members[1], result);

print("bring set back together");
replTest.unPartition(0,2);
replTest.unPartition(0,3);
replTest.unPartition(1,4);

master = replTest.getMaster();

print("make sure majority works");
result = testInsert();
assert.eq(result.err, null);
assert.eq(result.writtenTo.length, 3);
assert.contains(config.members[0], result);
assert.contains(config.members[1], result);
assert.contains(config.members[2], result);

replTest.stopSet();
}