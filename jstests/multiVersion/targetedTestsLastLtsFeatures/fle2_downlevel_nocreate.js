/**
 * Tests that the cluster cannot be downgraded when encrypted fields present
 *
 * @tags: [
 * requires_fcv_60
 * ]
 */

load("jstests/fle2/libs/encrypted_client_util.js");

(function() {
"use strict";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
rst.awaitReplication();

let dbName = 'downgrade_test';
let conn = rst.getPrimary();
let db = conn.getDB("admin");
let client = new EncryptedClient(conn, dbName);
let dbTest = db.getSiblingDB('create_encrypted_collection_db');

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

assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
assert.commandFailedWithCode(
    dbTest.createCollection("basic2", {encryptedFields: sampleEncryptedFields}),
    6662201,
    "Create in 5.0 FCV passed");

assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
assert.commandWorked(dbTest.createCollection("basic", {encryptedFields: sampleEncryptedFields}));

rst.stopSet();
}());
