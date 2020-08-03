// Check that rotation works for the cluster certificate in a sharded cluster

(function() {
"use strict";

load('jstests/ssl/libs/ssl_helpers.js');

if (determineSSLProvider() === "openssl") {
    return;
}

let mongos;
function getConnPoolHosts() {
    const ret = mongos.adminCommand({connPoolStats: 1});
    assert.commandWorked(ret);
    jsTestLog("Connection pool stats by host: " + tojson(ret.hosts));
    return ret.hosts;
}

const dbPath = MongoRunner.toRealDir("$dataDir/cluster_x509_rotate_test/");
mkdir(dbPath);

copyCertificateFile("jstests/libs/ca.pem", dbPath + "/ca-test.pem");
copyCertificateFile("jstests/libs/client.pem", dbPath + "/client-test.pem");
copyCertificateFile("jstests/libs/server.pem", dbPath + "/server-test.pem");

// server certificate is held constant so that shell can still connect
// we start a cluster using the old certificates, then rotate one shard to use new certificates.
// Make sure that mongos can communicate with every connected host EXCEPT that shard before a
// rotate, and make sure it can communicate with ONLY that shard after a rotate.
const mongosOptions = {
    sslMode: "requireSSL",
    sslPEMKeyFile: "jstests/libs/server.pem",
    sslCAFile: dbPath + "/ca-test.pem",
    sslClusterFile: dbPath + "/client-test.pem",
    sslAllowInvalidHostnames: "",
};

const configOptions = {
    sslMode: "requireSSL",
    sslPEMKeyFile: dbPath + "/server-test.pem",
    sslCAFile: dbPath + "/ca-test.pem",
    sslAllowInvalidHostnames: "",
};

const sharding_config = {
    config: 1,
    mongos: 1,
    shards: 3,
    other: {
        configOptions: configOptions,
        mongosOptions: mongosOptions,
        rsOptions: configOptions,
        shardOptions: configOptions,
    }
};

const st = new ShardingTest(sharding_config);

mongos = st.s0;

// Keep track of the hosts we hit in the initial ping multicast to compare against later multicasts
let output = mongos.adminCommand({multicast: {ping: 0}});
assert.eq(output.ok, 1);

let keys = [];
for (let key in output.hosts) {
    keys.push(key);
}

const rst = st.rs0;
const primary = rst.getPrimary();

// Swap out the certificate files and rotate the primary shard.
copyCertificateFile("jstests/libs/trusted-ca.pem", dbPath + "/ca-test.pem");
copyCertificateFile("jstests/libs/trusted-client.pem", dbPath + "/client-test.pem");
copyCertificateFile("jstests/libs/trusted-server.pem", dbPath + "/server-test.pem");

assert.commandWorked(primary.adminCommand({rotateCertificates: 1}));

// Make sure the primary is initially present
assert(primary.host in getConnPoolHosts());

// Drop connection to all hosts to see what we can reconnect to
assert.commandWorked(mongos.adminCommand({dropConnections: 1, hostAndPort: keys}));
assert(!(primary.host in getConnPoolHosts()));

assert.soon(() => {
    output = mongos.adminCommand({multicast: {ping: 0}});
    jsTestLog("Multicast 1 output: " + tojson(output));
    // multicast should fail, because the primary shard isn't hit
    if (output.ok !== 0) {
        return false;
    }
    for (let host in output.hosts) {
        if (host === primary.host) {
            if (output.hosts[host].ok !== 0) {
                return false;
            }
        } else {
            if (output.hosts[host].ok !== 1) {
                return false;
            }
        }
    }
    for (let key of keys) {
        if (!(key in output.hosts)) {
            return false;
        }
    }
    return true;
});

// rotate, drop all connections, re-multicast and see what we now hit
assert.commandWorked(mongos.adminCommand({rotateCertificates: 1}));

mongos.adminCommand({dropConnections: 1, hostAndPort: keys});
assert(!(primary.host in getConnPoolHosts()));

assert.soon(() => {
    output = mongos.adminCommand({multicast: {ping: 0}});
    jsTestLog("Multicast 2 output: " + tojson(output));
    if (output.ok !== 0) {
        return false;
    }
    for (let host in output.hosts) {
        if (host === primary.host) {
            if (output.hosts[host].ok !== 1) {
                return false;
            }
        } else {
            if (output.hosts[host].ok !== 0) {
                return false;
            }
        }
    }
    for (let key of keys) {
        if (!(key in output.hosts)) {
            return false;
        }
    }
    return true;
});
// Don't call st.stop() -- breaks because cluster is only partially rotated (this is hard to fix)
return;
}());
