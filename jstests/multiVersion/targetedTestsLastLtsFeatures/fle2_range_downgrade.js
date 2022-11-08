/**
 * Tests that the cluster cannot be downgraded when range encrypted fields present
 *
 * @tags: [
 * requires_fcv_61
 * ]
 */

load("jstests/fle2/libs/encrypted_client_util.js");

(function() {
"use strict";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
rst.awaitReplication();

let conn = rst.getPrimary();
let db = conn.getDB("admin");

function runTest(targetFCV) {
    assert.commandWorked(db.createCollection("basic", {
        encryptedFields: {
            "fields": [
                {
                    "path": "first",
                    "keyId": UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
                    "bsonType": "int",
                    "queries": {"queryType": "rangePreview", "min": 1, "max": 2, "sparsity": 1}
                },
            ]
        }
    }));

    let res = assert.commandFailedWithCode(
        db.adminCommand({setFeatureCompatibilityVersion: targetFCV}), ErrorCodes.CannotDowngrade);

    assert(db.basic.drop());

    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: targetFCV}));
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
}

runTest(lastLTSFCV);
runTest(lastContinuousFCV);

rst.stopSet();
})();
