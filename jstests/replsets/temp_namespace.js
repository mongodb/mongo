// SERVER-10927
// This is to make sure that temp collections get cleaned up on promotion to primary

var replTest = new ReplSetTest({ name: 'testSet', nodes: 3 });
var nodes = replTest.nodeList();
printjson(nodes);

// We need an arbiter to ensure that the primary doesn't step down when we restart the secondary
replTest.startSet();
replTest.initiate({"_id" : "testSet",
                   "members" : [
                    {"_id" : 0, "host" : nodes[0]},
                    {"_id" : 1, "host" : nodes[1]},
                    {"_id" : 2, "host" : nodes[2], "arbiterOnly" : true}]});

var master = replTest.getMaster();
var second = replTest.getSecondary();

var masterId = replTest.getNodeId(master);
var secondId = replTest.getNodeId(second);

var masterDB = master.getDB('test');
var secondDB = second.getDB('test');

// set up collections
masterDB.runCommand({create: 'temp1', temp: true});
masterDB.temp1.ensureIndex({x:1});
masterDB.runCommand({create: 'temp2', temp: 1});
masterDB.temp2.ensureIndex({x:1});
masterDB.runCommand({create: 'keep1', temp: false});
masterDB.runCommand({create: 'keep2', temp: 0});
masterDB.runCommand({create: 'keep3'});
masterDB.keep4.insert({});
masterDB.getLastError(2);

// make sure they exist on primary and secondary
assert.eq(masterDB.system.namespaces.count({name: /temp\d$/}) , 2); // collections
assert.eq(masterDB.system.namespaces.count({name: /temp\d\.\$.*$/}) , 4); // indexes (2 _id + 2 x)
assert.eq(masterDB.system.namespaces.count({name: /keep\d$/}) , 4);

assert.eq(secondDB.system.namespaces.count({name: /temp\d$/}) , 2); // collections
assert.eq(secondDB.system.namespaces.count({name: /temp\d\.\$.*$/}) , 4); // indexes (2 _id + 2 x)
assert.eq(secondDB.system.namespaces.count({name: /keep\d$/}) , 4);

// restart secondary and reconnect
replTest.restart(secondId, {},  /*wait=*/true);

// wait for the secondary to achieve secondary status
assert.soon(function () {
                try {
                    res = second.getDB("admin").runCommand({ replSetGetStatus: 1 });
                    return res.myState == 2;
                }
                catch (e) {
                    return false;
                }
            }, "took more than a minute for the secondary to become secondary again", 60*1000);

// make sure restarting secondary didn't drop collections
assert.eq(secondDB.system.namespaces.count({name: /temp\d$/}) , 2); // collections
assert.eq(secondDB.system.namespaces.count({name: /temp\d\.\$.*$/}) , 4); // indexes (2 _id + 2 x)
assert.eq(secondDB.system.namespaces.count({name: /keep\d$/}) , 4);

// step down primary and make sure former secondary (now primary) drops collections
try {
    master.adminCommand({replSetStepDown: 50, force : true});
} catch (e) {
    // ignoring socket errors since they sometimes, but not always, fire after running that command.
}

assert.soon(function() {
    printjson(secondDB.adminCommand("replSetGetStatus"));
    printjson(secondDB.isMaster());
    return secondDB.isMaster().ismaster;
}, '',  75*1000); // must wait for secondary to be willing to promote self

assert.eq(secondDB.system.namespaces.count({name: /temp\d$/}) , 0); // collections
assert.eq(secondDB.system.namespaces.count({name: /temp\d\.\$.*$/}) , 0); //indexes
assert.eq(secondDB.system.namespaces.count({name: /keep\d$/}) , 4);

// check that former primary dropped collections
replTest.awaitReplication()
assert.eq(masterDB.system.namespaces.count({name: /temp\d$/}) , 0); // collections
assert.eq(masterDB.system.namespaces.count({name: /temp\d\.\$.*$/}) , 0); //indexes
assert.eq(masterDB.system.namespaces.count({name: /keep\d$/}) , 4);

replTest.stopSet();
