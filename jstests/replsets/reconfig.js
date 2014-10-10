
// try reconfiguring with servers down

var replTest = new ReplSetTest({ name: 'testSet', nodes: 5 });
var nodes = replTest.startSet();
replTest.initiate();

var master = replTest.getMaster();

print("initial sync");
master.getDB("foo").bar.insert({X:1});
replTest.awaitReplication();

print("invalid reconfig");
var config = master.getDB("local").system.replset.findOne();
config.version++;
config.members.push({_id : 5, host : "localhost:12345, votes:0"});
var result = master.adminCommand({replSetReconfig : config});
printjson(result);
assert.eq(result.ok, 0);

print("stopping 3 & 4");
replTest.stop(3);
replTest.stop(4);

print("reconfiguring");
master = replTest.getMaster();
config = master.getDB("local").system.replset.findOne();
var oldVersion = config.version++;
config.members[0].priority = 5;
try {
    assert.commandWorked(master.getDB("admin").runCommand({replSetReconfig : config}));
}
catch(e) {
    print(e);
}

assert.soon(function() {
    try {
        var config = master.getDB("local").system.replset.findOne();
        return oldVersion+1 == config.version;
    }
    catch (e) {
        print("Query failed: "+e);
        return false;
    }
});

replTest.stopSet();

replTest2 = new ReplSetTest({name : 'testSet2', nodes : 1});
nodes = replTest2.startSet();

assert.soon(function() {
    try {
        result = nodes[0].getDB("admin").runCommand({replSetInitiate : {_id : "testSet2", members : [
            {_id : 0, tags : ["member0"]}
        ]}});
        printjson(result);
        return (result.errmsg.match(/bad or missing host field/) ||
                result.errmsg.match(/Missing expected field \"host\"/));
    }
    catch (e) {
        print(e);
    }
    return false;
});

replTest2.stopSet();
