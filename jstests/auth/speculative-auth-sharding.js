// Verify that clients can speculatively authenticate to mongos.
// @tags: [requires_sharding]

(function() {
'use strict';

const fallbackMech = 'SCRAM-SHA-256';
const keyfile = 'jstests/libs/key1';
const st = new ShardingTest({
    mongos: 1,
    keyFile: keyfile,
    other: {mongosOptions: {auth: null}, configOptions: {auth: null}, shardOptions: {auth: null}}
});

const admin = st.s.getDB('admin');
admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
admin.auth('admin', 'pwd');

let lastStats =
    assert.commandWorked(admin.runCommand({serverStatus: 1})).security.authentication.mechanisms;
jsTest.log('Inintial stats: ' + lastStats);

function test(uri, incrMech) {
    jsTest.log('Connecting to: ' + uri);
    assert.eq(runMongoProgram('mongo', uri, '--eval', ';'), 0);

    const stats = assert.commandWorked(admin.runCommand({serverStatus: 1}))
                      .security.authentication.mechanisms;
    assert.eq(Object.keys(lastStats).length, Object.keys(stats).length);
    Object.keys(lastStats).forEach(function(mech) {
        const inc = (mech == incrMech) ? 1 : 0;

        const specBefore = lastStats[mech].speculativeAuthenticate;
        const specAfter = stats[mech].speculativeAuthenticate;
        assert.eq(specAfter.received, specBefore.received + inc);
        assert.eq(specAfter.successful, specBefore.successful + inc);

        const allBefore = lastStats[mech].authenticate;
        const allAfter = stats[mech].authenticate;
        assert.eq(allAfter.received, allBefore.received + inc);
        assert.eq(allAfter.successful, allBefore.successful + inc);
    });
    lastStats = stats;
}

const baseURI = 'mongodb://admin:pwd@' + st.s.host + '/admin';

test(baseURI, fallbackMech);
test(baseURI + '?authMechanism=SCRAM-SHA-1', 'SCRAM-SHA-1');
test(baseURI + '?authMechanism=SCRAM-SHA-256', 'SCRAM-SHA-256');

admin.logout();
st.stop();
}());
