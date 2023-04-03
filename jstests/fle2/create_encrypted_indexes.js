// Test restrictions on createIndexes if indexing on encrypted fields

/**
 * @tags: [
 * assumes_unsharded_collection,
 * requires_fcv_70
 * ]
 */
load("jstests/fle2/libs/encrypted_client_util.js");

(function() {
'use strict';

let dbTest = db.getSiblingDB('create_encrypted_indexes_db');

dbTest.basic.drop();

const sampleEncryptedFields = {
    "fields": [
        {
            "path": "firstName",
            "keyId": UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
            "bsonType": "string",
            "queries": {"queryType": "equality"}  // allow single object or array
        },
        {
            "path": "paymentMethods.creditCards.number",
            "keyId": UUID("12341234-1234-1234-1234-123412341234"),
            "bsonType": "string",
            "queries": {"queryType": "equality"}
        },
    ]
};

let res = null;
res = dbTest.createCollection("basic", {encryptedFields: sampleEncryptedFields});
assert.commandWorked(res);

// Test create TTL index fails on encrypted collection
res = dbTest.basic.createIndex({"firstName": 1}, {expireAfterSeconds: 10});
assert.commandFailedWithCode(res, 6346501, "Create TTL index on encrypted collection passed");

// Test create unique index fails on encrypted field
res = dbTest.basic.createIndex({"firstName": 1}, {unique: true});
assert.commandFailedWithCode(res, 6346502, "Create unique index on encrypted field passed");

// Test create unique index fails on a prefix of an encrypted field
res = dbTest.basic.createIndex({"paymentMethods.creditCards": 1}, {unique: true});
assert.commandFailedWithCode(
    res, 6346502, "Create unique index on prefix of encrypted field passed");

// Test create unique index fails if prefix is an encrypted field
res = dbTest.basic.createIndex({"paymentMethods.creditCards.number.lastFour": 1}, {unique: true});
assert.commandFailedWithCode(
    res, 6346502, "Create unique index on key with encrypted field prefix passed");

// Test create single-field index on an encrypted field or prefix of an encrypted field fails.
res = dbTest.basic.createIndex({"firstName": 1});
assert.commandFailedWithCode(res, 6346502, "Create index on prefix of encrypted field passed");

res = dbTest.basic.createIndex({"paymentMethods.creditCards": 1});
assert.commandFailedWithCode(res, 6346502, "Create index on prefix of encrypted field passed");

res = dbTest.basic.createIndex({"firstName.$**": 1});
assert.commandFailedWithCode(
    res, 6346502, "Create wildcard index on prefix of encrypted field passed");

// Test that a compound index cannot contain an encrypted field.
res = dbTest.basic.createIndex({"notEncrypted": 1, "paymentMethods.creditCards": 1});
assert.commandFailedWithCode(res, 6346502, "Create compound index on encrypted field passed");

// The index key may share a prefix with an encrypted field.
assert.commandWorked(dbTest.basic.createIndex({"paymentMethods.creditCards.notNumber": 1}));

// A wildcard index on the entire document is allowed.
assert.commandWorked(dbTest.basic.createIndex({"$**": 1}));
}());
