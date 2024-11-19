/**
 * Verify valid and invalid scenarios for shard collection
 *
 * @tags: [
 * requires_fcv_70
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

let dbName = 'shard_state';
let dbTest = db.getSiblingDB(dbName);
dbTest.dropDatabase();

let client = new EncryptedClient(db.getMongo(), dbName);

assert.commandWorked(client.createEncryptionCollection("basic", {
    encryptedFields:
        {"fields": [{"path": "first", "bsonType": "string", "queries": {"queryType": "equality"}}]}
}));

const result = dbTest.getCollectionInfos({name: "basic"});
print("result" + tojson(result));
const ef = result[0].options.encryptedFields;
assert.eq(ef.escCollection, "enxcol_.basic.esc");
assert.eq(ef.ecocCollection, "enxcol_.basic.ecoc");

assert.commandFailedWithCode(
    db.adminCommand({shardCollection: 'shard_state.enxcol_.basic.esc', key: {_id: 1}}), 6464401);
assert.commandFailedWithCode(
    db.adminCommand({shardCollection: 'shard_state.enxcol_.basic.ecoc', key: {_id: 1}}), 6464401);
