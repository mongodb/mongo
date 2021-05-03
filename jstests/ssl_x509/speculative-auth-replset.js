// Verify that replica sets can speculatively authenticate
// to each other during intra-cluster communication.
// @tags: [requires_replication]

(function() {
'use strict';

const x509_options = {
    tlsMode: 'requireTLS',
    tlsCertificateKeyFile: 'jstests/libs/server.pem',
    tlsCAFile: 'jstests/libs/ca.pem',
    clusterAuthMode: 'sendX509',
};

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: x509_options,

    // ReplSetTest needs a keyFile present in order to know we want intracluster auth.
    keyFile: 'jstests/libs/key1',
    // ReplicaSet needs to use localhost so that SAN/CN values match.
    useHostName: false,
});

rst.startSet();
rst.initiate();

const admin = rst.getPrimary().getDB('admin');
admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});

function getMechStats(db) {
    return assert.commandWorked(db.runCommand({serverStatus: 1}))
        .security.authentication.mechanisms;
}

authutil.assertAuthenticate(rst.getPrimary(), '$external', {
    mechanism: 'MONGODB-X509',
});

// Capture statistics after a fresh instantiation of a 1-node replica set.
const initialMechStats = getMechStats(admin);
printjson(initialMechStats);
assert(initialMechStats['MONGODB-X509'] !== undefined);

// We've made no client connections for which speculation was possible,
// because we authenticated as `admin` using the shell helpers with SCRAM.
// Because of the simple cluster topology, we should have no intracluster authentication attempts.
Object.keys(initialMechStats).forEach(function(mech) {
    const specStats = initialMechStats[mech].speculativeAuthenticate;
    const clusterStats = initialMechStats[mech].clusterAuthenticate;

    if (mech === 'MONGODB-X509') {
        assert.eq(clusterStats.received, 1);
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

    const newNode = rst.add(x509_options);
    rst.reInitiate();
    rst.waitForState(newNode, ReplSetTest.State.SECONDARY);

    rst.stop(newNode);
    rst.remove(newNode);

    singleNodeConfig.version = rst.getReplSetConfigFromNode(0).version + 1;
    assert.commandWorked(admin.runCommand({replSetReconfig: singleNodeConfig, force: true}));
}

{
    // Capture new statistics, and assert that they're consistent.
    const newMechStats = getMechStats(admin);
    printjson(newMechStats);
    assert.eq(newMechStats["MONGODB-X509"].speculativeAuthenticate.received,
              newMechStats["MONGODB-X509"].speculativeAuthenticate.successful);
    assert.eq(newMechStats["MONGODB-X509"].clusterAuthenticate.received,
              newMechStats["MONGODB-X509"].clusterAuthenticate.successful);

    // Speculative and cluster statistics should be incremented by intracluster auth.
    assert.gt(newMechStats["MONGODB-X509"].speculativeAuthenticate.received,
              initialMechStats["MONGODB-X509"].speculativeAuthenticate.successful);
    assert.gt(newMechStats["MONGODB-X509"].clusterAuthenticate.received,
              initialMechStats["MONGODB-X509"].clusterAuthenticate.successful);
}

admin.logout();
rst.stopSet();
}());
