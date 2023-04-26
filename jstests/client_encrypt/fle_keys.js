/**
 * Check functionality of KeyVault.js
 */

load('jstests/ssl/libs/ssl_helpers.js');

(function() {
"use strict";

const x509_options = {
    sslMode: "requireSSL",
    sslPEMKeyFile: SERVER_CERT,
    sslCAFile: CA_CERT
};

const conn = MongoRunner.runMongod(x509_options);
const test = conn.getDB("test");
const collection = test.coll;

const localKMS = {
    key: BinData(
        0,
        "tu9jUCBqZdwCelwE/EAm/4WqdxrSMi04B8e9uAV+m30rI1J2nhKZZtQjdvsSCwuI4erR6IEcEK+5eGUAODv43NDNIR9QheT2edWFewUfHKsl9cnzTc86meIzOmYl6drp"),
};

const clientSideFLEOptions = {
    kmsProviders: {
        local: localKMS,
    },
    keyVaultNamespace: "test.coll",
    schemaMap: {}
};

const conn_str = "mongodb://" + conn.host + "/?ssl=true";
const shell = Mongo(conn_str, clientSideFLEOptions);
const keyVault = shell.getKeyVault();

keyVault.createKey("local", ['mongoKey']);
assert.eq(1, keyVault.getKeys().itcount());

var result = keyVault.createKey("local", "fake", {});
assert.eq("TypeError: key alternate names must be of Array type.", result);

result = keyVault.createKey("local", [1]);
assert.eq("TypeError: items in key alternate names must be of String type.", result);

assert.eq(1, keyVault.getKeyByAltName("mongoKey").itcount());

var keyId = keyVault.getKeyByAltName("mongoKey").toArray()[0]._id;

keyVault.addKeyAlternateName(keyId, "mongoKey2");

assert.eq(1, keyVault.getKeyByAltName("mongoKey2").itcount());
assert.eq(2, keyVault.getKey(keyId).toArray()[0].keyAltNames.length);
assert.eq(1, keyVault.getKeys().itcount());

result = keyVault.addKeyAlternateName(keyId, [2]);
assert.eq("TypeError: key alternate name cannot be object or array type.", result);

// Test create key with no CMK
result = keyVault.createKey("aws", ["altName"]);
assert.eq("ValueError: customerMasterKey must be defined if kmsProvider is not local.", result);

keyVault.removeKeyAlternateName(keyId, "mongoKey2");
assert.eq(1, keyVault.getKey(keyId).toArray()[0].keyAltNames.length);

result = keyVault.deleteKey(keyId);
assert.eq(0, keyVault.getKey(keyId).itcount());
assert.eq(0, keyVault.getKeys().itcount());

keyVault.createKey("local", ['mongoKey1']);
keyVault.createKey("local", ['mongoKey2']);
keyVault.createKey("local", ['mongoKey3']);

assert.eq(3, keyVault.getKeys().itcount());

MongoRunner.stopMongod(conn);
}());
