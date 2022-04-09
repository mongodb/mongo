// Verify create encrypted collection fails on standalone

/**
 * @tags: [
 *  requires_fcv_60,
 *  assumes_against_mongod_not_mongos,
 *  no_selinux
 * ]
 */
(function() {
'use strict';

let dbTest = db.getSiblingDB('create_encrypted_collection_db');

dbTest.basic.drop();

const sampleEncryptedFields = {
    "fields": [
        {
            "path": "firstName",
            "keyId": UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
            "bsonType": "string",
            "queries": {"queryType": "equality"}  // allow single object or array
        },
    ]
};

assert.commandFailedWithCode(
    db.runCommand({create: "basic", encryptedFields: sampleEncryptedFields}),
    6346402,
    "Create with encryptedFields passed on standalone");
}());
