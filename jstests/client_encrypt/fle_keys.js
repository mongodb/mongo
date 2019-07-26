/**
 * Check functionality of KeyVault.js
 */

load("jstests/client_encrypt/lib/mock_kms.js");
load('jstests/ssl/libs/ssl_helpers.js');

(function() {
"use strict";

const mock_kms = new MockKMSServer();
mock_kms.start();

const x509_options = {
    sslMode: "requireSSL",
    sslPEMKeyFile: SERVER_CERT,
    sslCAFile: CA_CERT
};

const conn = MongoRunner.runMongod(x509_options);
const test = conn.getDB("test");
const collection = test.coll;

const awsKMS = {
    accessKeyId: "access",
    secretAccessKey: "secret",
    url: mock_kms.getURL(),
};

const clientSideFLEOptions = {
    kmsProviders: {
        aws: awsKMS,
    },
    keyVaultNamespace: "test.coll",
    schemaMap: {}
};

const conn_str = "mongodb://" + conn.host + "/?ssl=true";
const shell = Mongo(conn_str, clientSideFLEOptions);
const keyVault = shell.getKeyVault();

var key = keyVault.createKey("aws", "arn:aws:kms:us-east-1:fake:fake:fake", ['mongoKey']);
assert.eq(1, keyVault.getKeys().itcount());

var result = keyVault.createKey("aws", "arn:aws:kms:us-east-4:fake:fake:fake", {});
assert.eq("TypeError: key alternate names must be of Array type.", result);

result = keyVault.createKey("aws", "arn:aws:kms:us-east-5:fake:fake:fake", [1]);
assert.eq("TypeError: items in key alternate names must be of String type.", result);

assert.eq(1, keyVault.getKeyByAltName("mongoKey").itcount());

var keyId = keyVault.getKeyByAltName("mongoKey").toArray()[0]._id;

keyVault.addKeyAlternateName(keyId, "mongoKey2");

assert.eq(1, keyVault.getKeyByAltName("mongoKey2").itcount());
assert.eq(2, keyVault.getKey(keyId).toArray()[0].keyAltNames.length);
assert.eq(1, keyVault.getKeys().itcount());

result = keyVault.addKeyAlternateName(keyId, [2]);
assert.eq("TypeError: key alternate name cannot be object or array type.", result);

keyVault.removeKeyAlternateName(keyId, "mongoKey2");
assert.eq(1, keyVault.getKey(keyId).toArray()[0].keyAltNames.length);

result = keyVault.deleteKey(keyId);
assert.eq(0, keyVault.getKey(keyId).itcount());
assert.eq(0, keyVault.getKeys().itcount());

assert.writeOK(keyVault.createKey("aws", "arn:aws:kms:us-east-1:fake:fake:fake1"));
assert.writeOK(keyVault.createKey("aws", "arn:aws:kms:us-east-2:fake:fake:fake2"));
assert.writeOK(keyVault.createKey("aws", "arn:aws:kms:us-east-3:fake:fake:fake3"));

assert.eq(3, keyVault.getKeys().itcount());

MongoRunner.stopMongod(conn);
mock_kms.stop();
}());