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
    nodes: 3,
    nodeOptions: x509_options,

    // ReplSetTest needs a keyFile present in order to know we want intracluster auth.
    keyFile: 'jstests/libs/key1',
    // ReplicaSet needs to use localhost so that SAN/CN values match.
    useHostName: false,
});

rst.startSet();
rst.initiate();
rst.awaitSecondaryNodes();

const admin = rst.getPrimary().getDB('admin');
admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
admin.auth('admin', 'pwd');

// We should have non-zero MONGODB-X509 successes using internal auth.
// And we should have no other types of speculative authentications.
const mechStats =
    assert.commandWorked(admin.runCommand({serverStatus: 1})).security.authentication.mechanisms;
printjson(mechStats);
assert(mechStats['MONGODB-X509'] !== undefined);
Object.keys(mechStats).forEach(function(mech) {
    const specStats = mechStats[mech].speculativeAuthenticate;
    const clusterStats = mechStats[mech].clusterAuthenticate;
    if (mech === 'MONGODB-X509') {
        assert.gte(specStats.received, 2);
        assert.gte(clusterStats.received, 2);
    } else {
        assert.eq(specStats.received, 0);
    }
    assert.eq(specStats.received, specStats.successful);
    assert.gte(clusterStats.received, clusterStats.successful);
});

admin.logout();
rst.stopSet();
}());
