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
assert.writeOK(master.getDB("foo").bar.insert({x:1}));
replSet.awaitReplication();

var primary = master.getDB("foo");
replSet.nodes[1].setSlaveOk();
replSet.nodes[2].setSlaveOk();
var A = replSet.nodes[1].getDB("admin");
var B = replSet.nodes[2].getDB("admin");
var primaryAddress = getHostName()+":"+replSet.ports[0];
var bAddress = getHostName()+":"+replSet.ports[2];

print("Force A to sync from B");
assert.soon(
    function() {
        if (A.runCommand({replSetGetStatus : 1}).syncingTo === bAddress) {
            return true;
        }
        A.runCommand({replSetSyncFrom : bAddress});
        return false;
    }, "A refused to sync from B", 30*1000, 1000
);

print("Black-hole and freeze B");
assert.commandWorked(B.runCommand({configureFailPoint: 'rsStopGetMore', mode: 'alwaysOn'}),
                     'failed to enable rsStopGetMore fail point');
assert.commandWorked(B.runCommand({configureFailPoint: 'rsStopGetMoreCmd', mode: 'alwaysOn'}),
                     'failed to enable rsStopGetMoreCmd fail point');
assert.commandWorked(B.runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'}),
                     'failed to enable rsSyncApplyStop fail point');

// Do another write, to make node 0 a viable sync source
assert.writeOK(master.getDB("foo").bar.insert({x:2}));

print("Check that A switches sync targets after 30 second socket timeout");

assert.soon(
    function() {
        return A.runCommand({replSetGetStatus : 1}).syncingTo === primaryAddress;
    }, "did not switch sync sources", 60000, 1000);

print("Un-black-hole B and make sure nothing stupid happens");
assert.commandWorked(B.runCommand({configureFailPoint: 'rsStopGetMore', mode: 'off'}),
                     'failed to disable rsStopGetMore fail point');
assert.commandWorked(B.runCommand({configureFailPoint: 'rsStopGetMoreCmd', mode: 'off'}),
                     'failed to disable rsStopGetMoreCmd fail point');
assert.commandWorked(B.runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'off'}),
                     'failed to disable rsSyncApplyStop fail point');
replSet.awaitReplication();
replSet.stopSet();
