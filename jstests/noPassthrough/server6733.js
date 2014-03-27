// Test that the oplog reader times out after 30 seconds with no response

print("Bring up three members");
var replSet = new ReplSetTest({name: 'testSet', nodes: 3});
replSet.startSet();
replSet.initiate(
    {
        _id:'testSet',
        members:
        [
            {_id: 0, host: getHostName()+":"+replSet.ports[0]},
            {_id: 1, host: getHostName()+":"+replSet.ports[1], priority: 0},
            {_id: 2, host: getHostName()+":"+replSet.ports[2], priority: 0}
        ]
    }
);

// Do an initial write
var master = replSet.getMaster();
master.getDB("foo").bar.insert({x:1});
replSet.awaitReplication();

var primary = master.getDB("foo");
replSet.nodes[1].setSlaveOk();
replSet.nodes[2].setSlaveOk();
var A = replSet.nodes[1].getDB("admin");
var B = replSet.nodes[2].getDB("admin");
var primaryAddress = getHostName()+":"+replSet.ports[0];
var bAddress = getHostName()+":"+replSet.ports[2];

print("Force A to sync from B");
A.runCommand({replSetSyncFrom : bAddress});
assert.soon(
    function() {
        return A.runCommand({replSetGetStatus : 1}).syncingTo == bAddress;
    }
);

print("Black-hole B");
B.runCommand({configureFailPoint: 'rsStopGetMore', mode: 'alwaysOn'});

print("Check that A switches sync targets after 30 seconds");
sleep(30000);

assert.soon(
    function() {
        return A.runCommand({replSetGetStatus : 1}).syncingTo == primaryAddress;
    }
);

print("Un-black-hole B and make sure nothing stupid happens");
B.runCommand({configureFailPoint: 'rsStopGetMore', mode: 'off'});

sleep(10000);

replSet.stopSet();
