// Check that rotation works for the server certificate

import {copyCertificateFile} from "jstests/ssl/libs/ssl_helpers.js";

const OLD_SERVER = getX509Path("server.pem");
const OLD_CLIENT = getX509Path("client.pem");
const OLD_CA = getX509Path("ca.pem");

const NEW_SERVER = getX509Path("trusted-server.pem");
const NEW_CLIENT = getX509Path("trusted-client.pem");
const NEW_CA = getX509Path("trusted-ca.pem");

const dbPath = MongoRunner.toRealDir("$dataDir/cluster_x509_rotate_test/");
mkdir(dbPath);

copyCertificateFile(OLD_CA, dbPath + "/ca-test.pem");
copyCertificateFile(OLD_SERVER, dbPath + "/server-test.pem");

const mongod = MongoRunner.runMongod({
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: dbPath + "/server-test.pem",
    tlsCAFile: dbPath + "/ca-test.pem",
});
const host = "localhost:" + mongod.port;

// Assert we can connect using old certificates.
const conn = new Mongo(host, undefined, {tls: {certificateKeyFile: OLD_CLIENT, CAFile: OLD_CA}});
assert.commandWorked(conn.adminCommand({connectionStatus: 1}));

// Rotate in new certificates
copyCertificateFile(NEW_CA, dbPath + "/ca-test.pem");
copyCertificateFile(NEW_SERVER, dbPath + "/server-test.pem");

assert.commandWorked(mongod.getDB("test").rotateCertificates("Rotated!"));
// make sure that mongo is still connected after rotation
assert.commandWorked(mongod.adminCommand({connectionStatus: 1}));

// Start shell with old certificates and make sure it can't connect
assert.throws(() => {
    new Mongo(host, undefined, {tls: {certificateKeyFile: OLD_CLIENT, CAFile: OLD_CA}});
});

// Start shell with new certificates and make sure it can connect
const conn2 = new Mongo(host, undefined, {tls: {certificateKeyFile: NEW_CLIENT, CAFile: NEW_CA}});
assert.commandWorked(conn2.adminCommand({connectionStatus: 1}));

MongoRunner.stopMongod(mongod);
