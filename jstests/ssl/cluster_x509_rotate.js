// Check that rotation works for the cluster certificate

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {copyCertificateFile} from "jstests/ssl/libs/ssl_helpers.js";

const dbPath = MongoRunner.toRealDir("$dataDir/cluster_x509_rotate_test/");
mkdir(dbPath);

copyCertificateFile(getX509Path("ca.pem"), dbPath + "/ca-test.pem");
copyCertificateFile(getX509Path("client.pem"), dbPath + "/client-test.pem");
copyCertificateFile(getX509Path("server.pem"), dbPath + "/server-test.pem");

// Make replset with old certificates, rotate to new certificates, and try to add
// a node with new certificates.
const rst = new ReplSetTest({nodes: 2});
rst.startSet({
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: dbPath + "/server-test.pem",
    tlsCAFile: dbPath + "/ca-test.pem",
    tlsClusterFile: dbPath + "/client-test.pem",
    tlsAllowInvalidHostnames: "",
});

rst.initiate();
rst.awaitReplication();

copyCertificateFile(getX509Path("trusted-ca.pem"), dbPath + "/ca-test.pem");
copyCertificateFile(getX509Path("trusted-client.pem"), dbPath + "/client-test.pem");
copyCertificateFile(getX509Path("trusted-server.pem"), dbPath + "/server-test.pem");

for (let node of rst.nodes) {
    assert.commandWorked(node.adminCommand({rotateCertificates: 1}));
}

const newnode = rst.add({
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: getX509Path("trusted-server.pem"),
    tlsCAFile: getX509Path("trusted-ca.pem"),
    tlsClusterFile: getX509Path("trusted-client.pem"),
    tlsAllowInvalidHostnames: "",
    // IMPORTANT: shell will not be able to talk to the new node due to cert rotation
    // therefore we set "waitForConnect:false" to ensure shell does not try to acess it
    waitForConnect: false,
});

// Emulate waitForConnect so we wait for new node to come up before killing rst
const host = "localhost:" + newnode.port;
assert.soon(() => {
    print(`Testing that ${host} is up`);
    try {
        new Mongo(host, undefined, {
            tls: {
                certificateKeyFile: getX509Path("trusted-client.pem"),
                CAFile: getX509Path("trusted-ca.pem"),
                allowInvalidHostnames: true,
            },
        });
    } catch (error) {
        return false;
    }
    return true;
});

print("Reinitiating replica set");
rst.reInitiate();

assert.soon(() => {
    print(`Waiting for ${host} to join replica set`);
    try {
        const conn = new Mongo(host, undefined, {
            tls: {
                certificateKeyFile: getX509Path("trusted-client.pem"),
                CAFile: getX509Path("trusted-ca.pem"),
                allowInvalidHostnames: true,
            },
        });
        const PRIMARY = 1;
        const SECONDARY = 2;
        const s = conn.adminCommand({replSetGetStatus: 1});
        if (!s.ok) {
            print("replSetGetStatus is not ok");
            return false;
        }
        if (s.myState != PRIMARY && s.myState != SECONDARY) {
            print("node is not primary or secondary");
            return false;
        }
        print("node is online and in cluster");
    } catch (error) {
        return false;
    }
    return true;
});

// Make sure each node can connect to each other node
for (let node of rst.nodeList()) {
    for (let target of rst.nodeList()) {
        if (node !== target) {
            print(`Testing connectivity of ${node} to ${target}`);
            const conn = new Mongo(node, undefined, {
                tls: {
                    certificateKeyFile: getX509Path("trusted-client.pem"),
                    CAFile: getX509Path("trusted-ca.pem"),
                    allowInvalidHostnames: true,
                },
            });
            assert.commandWorked(conn.adminCommand({replSetTestEgress: 1, target, timeoutSecs: NumberInt(15)}));
        }
    }
}

rst.stopSet();
