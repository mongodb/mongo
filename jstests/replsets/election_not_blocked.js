/* Check that the fsyncLock'ed secondary will not veto an election
 *
 * 1. start a three node set with a hidden, priority:0 node which we will fsyncLock
 * 2. do a write to master
 * 3. fsyncLock the hidden, priority:0 node
 * 4. stepdown master
 * 5. wait for the non-fsyncLocked node to become master
 */

(function() {
    "use strict";
    var name = "electionNotBlocked";
    var replTest = new ReplSetTest({name: name, nodes: 3});
    var host = replTest.host;
    var nodes = replTest.startSet();
    var port = replTest.ports;
    replTest.initiate({
        _id: name,
        members: [
            {_id: 0, host: host + ":" + port[0], priority: 3},
            {_id: 1, host: host + ":" + port[1]},
            {_id: 2, host: host + ":" + port[2], hidden: true, priority: 0},
        ],
        // In PV1, a voter writes the last vote to disk before granting the vote,
        // so it cannot vote while fsync locked in PV1. Use PV0 explicitly here.
        protocolVersion: 0
    });
    replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);
    var master = replTest.getPrimary();

    // do a write
    assert.writeOK(master.getDB("foo").bar.insert({x: 1}, {writeConcern: {w: 3}}));

    var slave = replTest.liveNodes.slaves[0];
    // lock secondary
    var locked = replTest.liveNodes.slaves[1];
    locked.getDB("admin").fsyncLock();

    // take down master
    replTest.stop(0);

    replTest.waitForState(slave, ReplSetTest.State.PRIMARY, 90 * 1000);

    locked.getDB("admin").fsyncUnlock();
    replTest.stopSet();
}());
