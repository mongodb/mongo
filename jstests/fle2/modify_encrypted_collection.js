// Verify valid and invalid scenarios for collMod on an encrypted collection

/**
 * @tags: [
 * assumes_unsharded_collection,
 * requires_fcv_70
 * ]
 */
load("jstests/fle2/libs/encrypted_client_util.js");

(function() {
'use strict';

let dbTest = db.getSiblingDB('modify_encrypted_collection_db');

dbTest.basic.drop();

const sampleEncryptedFields = {
    "fields": [
        {
            "path": "firstName",
            "keyId": UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
            "bsonType": "string",
            "queries": {"queryType": "equality"}
        },
    ]
};

assert.commandWorked(dbTest.createCollection("basic", {encryptedFields: sampleEncryptedFields}));

assert.commandFailedWithCode(dbTest.runCommand({collMod: "basic", validationAction: "warn"}),
                             ErrorCodes.BadValue);

assert.commandFailedWithCode(dbTest.runCommand({collMod: "basic", validationLevel: "off"}),
                             ErrorCodes.BadValue);

assert.commandFailedWithCode(dbTest.runCommand({collMod: "basic", validationLevel: "moderate"}),
                             ErrorCodes.BadValue);

assert.commandWorked(
    dbTest.runCommand({collMod: "basic", validationLevel: "strict", validationAction: "error"}));
}());
