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

function runTest(targetFCV) {
    assert.commandWorked(client.createEncryptionCollection("basic", {
        encryptedFields: {
            "fields": [
                {"path": "first", "bsonType": "string", "queries": {"queryType": "equality"}},
                {"path": "middle", "bsonType": "string"},
                {"path": "aka", "bsonType": "string", "queries": {"queryType": "equality"}},
            ]
        }
    }));

    let res = assert.commandFailedWithCode(
        db.adminCommand({setFeatureCompatibilityVersion: targetFCV}), ErrorCodes.CannotDowngrade);

    assert(client.getDB().fle2.basic.ecoc.drop());
    assert(client.getDB().fle2.basic.ecc.drop());
    assert(client.getDB().fle2.basic.esc.drop());
    assert(client.getDB().basic.drop());

    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: targetFCV}));
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
}

targetFCV(lastLTSFCV);
targetFCV(lastContinuousFCV);

rst.stopSet();
}());