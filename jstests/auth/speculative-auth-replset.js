// Verify that replica sets can speculatively authenticate
// to each other during intra-cluster communication.
// @tags: [requires_replication]

(function() {
'use strict';

const rst = new ReplSetTest({nodes: 3, keyFile: 'jstests/libs/key1'});
rst.startSet();
rst.initiate();
rst.awaitSecondaryNodes();

const admin = rst.getPrimary().getDB('admin');
admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
admin.auth('admin', 'pwd');

const baseURI = (function() {
    let uri = 'mongodb://admin:pwd@';

    for (let i = 0; i < rst.ports.length; ++i) {
        if (i > 0) {
            uri = uri + ',';
        }
        uri = uri + rst.host + ':' + rst.ports[i];
    }

    return uri + '/admin?replicaSet=' + rst.name;
})();

function test(uri) {
    assert.eq(runMongoProgram('mongo', uri, '--eval', ';'), 0);
}

// We've made no client connections for which speculation was possible,
// since this connection came in during localhost auth bypass.
// However we should have non-zero SCRAM-SHA-256 successes using internal auth.
const mechStats =
    assert.commandWorked(admin.runCommand({serverStatus: 1})).security.authentication.mechanisms;
printjson(mechStats);
assert(mechStats['SCRAM-SHA-256'] !== undefined);
Object.keys(mechStats).forEach(function(mech) {
    const specStats = mechStats[mech].speculativeAuthenticate;
    const clusterStats = mechStats[mech].clusterAuthenticate;

    if (mech === 'SCRAM-SHA-256') {
        assert.gte(specStats.received, 2);
        assert.gte(clusterStats.received, 2);
    } else {
        assert.eq(specStats.received, 0);
    }
    assert.eq(specStats.received, specStats.successful);
    assert.eq(clusterStats.received, clusterStats.successful);
});

test(baseURI);
test(baseURI + '&authMechanism=SCRAM-SHA-1');
test(baseURI + '&authMechanism=SCRAM-SHA-256');

admin.logout();
rst.stopSet();
}());
