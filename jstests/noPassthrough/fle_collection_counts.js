/**
 * Validate csfle1 and qe collections are counted correctly.
 */

import {EncryptedClient, isEnterpriseShell} from "jstests/fle2/libs/encrypted_client_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

if (!isEnterpriseShell()) {
    jsTestLog("Skipping test as it requires the enterprise module");
    quit();
}

let dbName = jsTestName();

function assertCollectionCounts(db, qe, fle1) {
    const cs = db.serverStatus().catalogStats;

    assert.eq(cs.csfle, fle1, "Mismatch in actual vs expected for csfle1 collections");
    assert.eq(cs.queryableEncryption,
              qe,
              "Mismatch in actual vs expected for queryableEncryption collections");
}

function runTest(conn) {
    let db = conn.getDB(dbName);
    db.dropDatabase();

    let client = new EncryptedClient(conn, dbName);

    assertCollectionCounts(db, 0, 0);

    // Check we count correctly.
    assert.commandWorked(client.createEncryptionCollection("basic_qe", {
        encryptedFields: {
            "fields": [
                {"path": "first", "bsonType": "string", "queries": {"queryType": "equality"}},
                {"path": "middle", "bsonType": "string"},
                {"path": "aka", "bsonType": "string", "queries": {"queryType": "equality"}},
            ]
        }
    }));

    assertCollectionCounts(db, 1, 0);

    const defaultKeyId = client.runEncryptionOperation(() => {
        return client.getKeyVault().createKey("local", "ignored");
    });
    const schema = {
        encryptMetadata: {
            algorithm: "AEAD_AES_256_CBC_HMAC_SHA_512-Deterministic",
            keyId: [defaultKeyId],
        },
        type: "object",
        properties: {ssn: {encrypt: {bsonType: "string"}}}
    };

    let edb = client.getDB();

    // In FLE 1, encrypted collections are defined by their jsonSchema validator.
    assert.commandWorked(edb.runCommand({
        create: "basic_csfle1",
        validator: {$jsonSchema: schema},
    }));

    // Check it is decremented
    //
    assertCollectionCounts(db, 1, 1);

    edb.basic_qe.drop();

    assertCollectionCounts(db, 0, 1);

    edb.basic_csfle1.drop();

    assertCollectionCounts(db, 0, 0);
}

// Run test
const rst = new ReplSetTest({nodes: 1});
rst.startSet();

rst.initiate();
rst.awaitReplication();
runTest(rst.getPrimary());
rst.stopSet();
