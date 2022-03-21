// Verify compact collection capability in client side

/**
 * @tags: [
 *  featureFlagFLE2,
 * ]
 */
load("jstests/fle2/libs/encrypted_client_util.js");

(function() {
'use strict';

if (!isFLE2ReplicationEnabled()) {
    return;
}

const dbName = 'compact_collection_db';
const dbTest = db.getSiblingDB(dbName);
dbTest.dropDatabase();

const client = new EncryptedClient(db.getMongo(), dbName);
const edb = client.getDB();

const sampleEncryptedFields = {
    fields: [
        {path: "firstName", bsonType: "string", queries: {"queryType": "equality"}},
        {path: "a.b.c", bsonType: "int", queries: {"queryType": "equality"}},
    ]
};

assert.commandWorked(
    client.createEncryptionCollection("encrypted", {encryptedFields: sampleEncryptedFields}));
assert.commandWorked(edb.createCollection("unencrypted"));

assert.commandFailedWithCode(edb.unencrypted.compact(), ErrorCodes.BadValue);

const res = edb.encrypted.compact();
assert.commandWorked(res);
assert(res.hasOwnProperty("stats"));
assert(res.stats.hasOwnProperty("esc"));
assert(res.stats.hasOwnProperty("ecc"));
assert(res.stats.hasOwnProperty("ecoc"));
}());
