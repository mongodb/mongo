// Verify that replica sets can speculatively authenticate
// to each other during intra-cluster communication.
// @tags: [requires_replication]

(function() {
'use strict';

const kAuthenticationSuccessfulLogId = 5286306;
const kAuthenticationFailedLogId = 5286307;

function countAuthInLog(conn) {
    let logCounts = {speculative: 0, cluster: 0, speculativeCluster: 0};

    checkLog.getGlobalLog(conn).forEach((line) => {
        // Iterate through the log and verify our auth.
        const entry = JSON.parse(line);
        if (entry.id === kAuthenticationSuccessfulLogId) {
            // Successful auth.
            if (entry.attr.isSpeculative) {
                logCounts.speculative += 1;
            }
            if (entry.attr.isClusterMember) {
                logCounts.cluster += 1;
            }
            if (entry.attr.isSpeculative && entry.attr.isClusterMember) {
                logCounts.speculativeCluster += 1;
            }
        } else if (entry.id === kAuthenticationFailedLogId) {
            // Authentication can fail legitimately because the secondary abandons the connection
            // during shutdown.
            assert.eq(entry.attr.error.code, ErrorCodes.AuthenticationAbandoned);
        } else {
            // Irrelevant.
            return;
        }
    });

    print(`Found log entries for authentication in the following amounts: ${tojson(logCounts)}`);
    return logCounts;
}

const rst = new ReplSetTest({nodes: 1, keyFile: 'jstests/libs/key1'});
rst.startSet();
rst.initiate();
rst.awaitReplication();

const admin = rst.getPrimary().getDB('admin');
admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
admin.auth('admin', 'pwd');
assert.commandWorked(admin.setLogLevel(3, 'accessControl'));

function getMechStats(db) {
    return assert.commandWorked(db.runCommand({serverStatus: 1}))
        .security.authentication.mechanisms;
}

// Capture statistics after a fresh instantiation of a 1-node replica set.
const initialMechStats = getMechStats(admin);
printjson(initialMechStats);
assert(initialMechStats['SCRAM-SHA-256'] !== undefined);

// We've made no client connections for which speculation was possible,
// because we authenticated as `admin` using the shell helpers.
// Because of the simple cluster topology, we should have no intracluster authentication attempts.
Object.keys(initialMechStats).forEach(function(mech) {
    const specStats = initialMechStats[mech].speculativeAuthenticate;
    const clusterStats = initialMechStats[mech].clusterAuthenticate;

    if (mech === 'SCRAM-SHA-256') {
        // It appears that replication helpers use SCRAM-SHA-1, preventing SCRAM-SHA-256 cluster
        // stats from being incremented during test setup.
        assert.eq(clusterStats.received, 0);
    }

    // No speculation has occured
    assert.eq(specStats.received, 0);

    // Statistics should be consistent for all mechanisms
    assert.eq(specStats.received, specStats.successful);
    assert.eq(clusterStats.received, clusterStats.successful);
});

{
    // Add and remove a node to force intra-cluster traffic, and authentication attempts.
    // Removal will require force-reconfig because the original node will not constitute a
    // "majority" of the resulting two node replicaset.
    const singleNodeConfig = rst.getReplSetConfigFromNode();

    const newNode = rst.add({});
    rst.reInitiate();
    rst.waitForState(newNode, ReplSetTest.State.SECONDARY);
    rst.awaitReplication();

    rst.stop(newNode);
    rst.remove(newNode);
    admin.auth('admin', 'pwd');
    singleNodeConfig.version = rst.getReplSetConfigFromNode(0).version + 1;
    assert.commandWorked(admin.runCommand({replSetReconfig: singleNodeConfig, force: true}));
    rst.awaitReplication();
}

{
    // Capture new statistics, and assert that they're consistent.
    const newMechStats = getMechStats(admin);
    printjson(newMechStats);

    // Speculative and cluster statistics should be incremented by intracluster auth.
    assert.gt(newMechStats["SCRAM-SHA-256"].speculativeAuthenticate.successful,
              initialMechStats["SCRAM-SHA-256"].speculativeAuthenticate.successful);
    assert.gt(newMechStats["SCRAM-SHA-256"].clusterAuthenticate.successful,
              initialMechStats["SCRAM-SHA-256"].clusterAuthenticate.successful);

    const logCounts = countAuthInLog(admin);
    assert.eq(logCounts.speculative,
              newMechStats["SCRAM-SHA-256"].speculativeAuthenticate.successful);
    assert.eq(logCounts.cluster, newMechStats["SCRAM-SHA-256"].clusterAuthenticate.successful);
    assert.gt(logCounts.speculativeCluster,
              0,
              "Expected to observe at least one speculative cluster authentication attempt");
}

admin.logout();
rst.stopSet();
}());
