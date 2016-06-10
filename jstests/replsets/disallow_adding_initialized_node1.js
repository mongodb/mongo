// If a node is already in an active replica set, it is not possible to add this node to another
// replica set.
// Initialize two replica sets A and B with the same name: A_0; B_0
// Add B_0 to the replica set A. This operation should fail on replica set A should fail on
// detecting an inconsistent replica set ID in the heartbeat response metadata from B_0.
(function() {
    'use strict';

    var name = 'disallow_adding_initialized_node1';
    var replSetA = new ReplSetTest({
        name: name,
        nodes: [
            {rsConfig: {_id: 10}},
        ]
    });
    replSetA.startSet({dbpath: "$set-A-$node"});
    replSetA.initiate();

    var replSetB = new ReplSetTest({
        name: name,
        nodes: [
            {rsConfig: {_id: 20}},
        ]
    });
    replSetB.startSet({dbpath: "$set-B-$node"});
    replSetB.initiate();

    var primaryA = replSetA.getPrimary();
    var primaryB = replSetB.getPrimary();
    jsTestLog('Before merging: primary A = ' + primaryA.host + '; primary B = ' + primaryB.host);

    var configA = assert.commandWorked(primaryA.adminCommand({replSetGetConfig: 1})).config;
    var configB = assert.commandWorked(primaryB.adminCommand({replSetGetConfig: 1})).config;
    assert(configA.settings.replicaSetId instanceof ObjectId);
    assert(configB.settings.replicaSetId instanceof ObjectId);
    jsTestLog('Replica set A ID = ' + configA.settings.replicaSetId);
    jsTestLog('Replica set B ID = ' + configB.settings.replicaSetId);
    assert.neq(configA.settings.replicaSetId, configB.settings.replicaSetId);

    jsTestLog("Adding replica set B's primary " + primaryB.host + " to replica set A's config");
    configA.version++;
    configA.members.push({_id: 11, host: primaryB.host});
    var reconfigResult =
        assert.commandFailedWithCode(primaryA.adminCommand({replSetReconfig: configA}),
                                     ErrorCodes.NewReplicaSetConfigurationIncompatible);
    var msgA = 'Our replica set ID of ' + configA.settings.replicaSetId +
        ' did not match that of ' + primaryB.host + ', which is ' + configB.settings.replicaSetId;
    assert.neq(-1, reconfigResult.errmsg.indexOf(msgA));

    var newPrimaryA = replSetA.getPrimary();
    var newPrimaryB = replSetB.getPrimary();
    jsTestLog('After merging: primary A = ' + newPrimaryA.host + '; primary B = ' +
              newPrimaryB.host);
    assert.eq(primaryA, newPrimaryA);
    assert.eq(primaryB, newPrimaryB);

    // Mismatch replica set IDs in heartbeat responses should be logged.
    var checkLog = function(node, msg) {
        assert.soon(function() {
            var logMessages = assert.commandWorked(node.adminCommand({getLog: 'global'})).log;
            for (var i = 0; i < logMessages.length; i++) {
                if (logMessages[i].indexOf(msg) != -1) {
                    return true;
                }
            }
            return false;
        }, 'Did not see a log entry containing the following message: ' + msg, 60000, 1000);
    };
    var msgB = "replica set IDs do not match, ours: " + configB.settings.replicaSetId +
        "; remote node's: " + configA.settings.replicaSetId;
    checkLog(primaryB, msgB);

    var statusA = assert.commandWorked(primaryA.adminCommand({replSetGetStatus: 1}));
    var statusB = assert.commandWorked(primaryB.adminCommand({replSetGetStatus: 1}));
    jsTestLog('After merging: replica set status A = ' + tojson(statusA));
    jsTestLog('After merging: replica set status B = ' + tojson(statusB));

    // Replica set A's config should remain unchanged due to failed replSetReconfig command.
    assert.eq(1, statusA.members.length);
    assert.eq(10, statusA.members[0]._id);
    assert.eq(primaryA.host, statusA.members[0].name);
    assert.eq(ReplSetTest.State.PRIMARY, statusA.members[0].state);

    // Replica set B's config should remain unchanged.
    assert.eq(1, statusB.members.length);
    assert.eq(20, statusB.members[0]._id);
    assert.eq(primaryB.host, statusB.members[0].name);
    assert.eq(ReplSetTest.State.PRIMARY, statusB.members[0].state);

    replSetB.stopSet();
    replSetA.stopSet();
})();
