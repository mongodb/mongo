/**
 * Tests the upsert query does not crash. This was due to a bug where IDHack was searching the _id
 * index for a KeyString encoding of '{$eq: 1}', rather than '1'.
 * @tags: [
 * assumes_unsharded_collection,
 * requires_fcv_80
 * ]
 */
import {isMongos} from "jstests/concurrency/fsm_workload_helpers/server_types.js";
import {EncryptedClient, isEnterpriseShell} from "jstests/fle2/libs/encrypted_client_util.js";

// Passthrough workaround
if (!isMongos(db)) {
    quit();
}

if (!isEnterpriseShell()) {
    quit();
}

let dbName = 'test';
let collName = 'test';
let dbTest = db.getSiblingDB(dbName);
dbTest.dropDatabase();
let client = new EncryptedClient(db.getMongo(), dbName);
assert.commandWorked(client.createEncryptionCollection(collName, {
    encryptedFields: {
        "fields": [
            {"path": "a", "bsonType": "string", "queries": {"queryType": "equality"}},
        ]
    }
}));

const ecoll = client.getDB().getCollection(collName);
assert.commandWorked(ecoll.einsert({_id: 1, "a": "a"}));
assert.commandWorked(ecoll.eupdate({_id: 1}, {$set: {"a": "b"}}, true /*upsert*/, false /*multi*/));
