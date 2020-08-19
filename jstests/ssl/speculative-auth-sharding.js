// Verify that speculative auth works with mongos.
// @tags: [requires_sharding]

(function() {
'use strict';

const CLIENT_NAME = 'CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US';
const CLIENT_CERT = 'jstests/libs/client.pem';
const SERVER_CERT = 'jstests/libs/server.pem';
const CLUSTER_CERT = 'jstests/libs/cluster_cert.pem';
const CA_CERT = 'jstests/libs/ca.pem';

const options = {
    tlsMode: 'requireTLS',
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
    tlsClusterFile: CLUSTER_CERT,
    tlsAllowInvalidHostnames: '',
    clusterAuthMode: 'x509',
};

const st = new ShardingTest({
    shards: 1,
    other: {
        enableBalancer: true,
        configOptions: options,
        mongosOptions: options,
        rsOptions: options,
        shardOptions: options,
        shardAsReplicaSet: false,
    }
});

const admin = st.s.getDB('admin');
admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
assert(admin.auth('admin', 'pwd'));

const external = st.s.getDB('$external');
external.createUser({user: CLIENT_NAME, roles: [{role: '__system', db: 'admin'}]});

const initialStats = assert.commandWorked(admin.runCommand({serverStatus: 1}))
                         .security.authentication.mechanisms['MONGODB-X509'];
jsTest.log('Initial stats: ' + tojson(initialStats));

const uri = 'mongodb://' + st.s.host + '/admin?authMechanism=MONGODB-X509';
jsTest.log('Connecting to: ' + uri);
assert.eq(runMongoProgram('mongo',
                          uri,
                          '--tls',
                          '--tlsCertificateKeyFile',
                          CLIENT_CERT,
                          '--tlsCAFile',
                          CA_CERT,
                          '--tlsAllowInvalidHostnames',
                          '--eval',
                          ';'),
          0);

const authStats = assert.commandWorked(admin.runCommand({serverStatus: 1}))
                      .security.authentication.mechanisms['MONGODB-X509'];
jsTest.log('Authenticated stats: ' + tojson(authStats));

// Got and succeeded an additional speculation.
const initSpec = initialStats.speculativeAuthenticate;
const authSpec = authStats.speculativeAuthenticate;
assert.eq(authSpec.received, initSpec.received + 1);
assert.eq(authSpec.successful, initSpec.successful + 1);

// Got and succeeded an additional auth.
const initAuth = initialStats.authenticate;
const authAuth = authStats.authenticate;
assert.eq(authAuth.received, initAuth.received + 1);
assert.eq(authAuth.successful, initAuth.successful + 1);

/////////////////////////////////////////////////////////////////////////////

jsTest.log('Shutting down');

// Authenticate csrs so ReplSetTest.stopSet() can do db hash check.
if (st.configRS) {
    st.configRS.nodes.forEach((node) => {
        node.getDB('admin').auth('admin', 'pwd');
    });
}

// Orphan checks needs a privileged user to auth as.
st.shard0.getDB('$external')
    .createUser({user: CLIENT_NAME, roles: [{role: '__system', db: 'admin'}]});

st.stop();
}());
