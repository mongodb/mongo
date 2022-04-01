/**
 * Verify valid and invalid scenarios for shard collection
 *
 * @tags: [
 *  featureFlagFLE2,
 * ]
 */
load("jstests/fle2/libs/encrypted_client_util.js");

(function() {
'use strict';

if (!isFLE2ShardingEnabled()) {
    return;
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
assert.eq(ef.escCollection, "fle2.basic.esc");
assert.eq(ef.eccCollection, "fle2.basic.ecc");
assert.eq(ef.ecocCollection, "fle2.basic.ecoc");

assert.commandFailedWithCode(
    db.adminCommand({shardCollection: 'shard_state.fle2.basic.esc', key: {_id: 1}}), 6464401);
assert.commandFailedWithCode(
    db.adminCommand({shardCollection: 'shard_state.fle2.basic.ecc', key: {_id: 1}}), 6464401);
assert.commandFailedWithCode(
    db.adminCommand({shardCollection: 'shard_state.fle2.basic.ecoc', key: {_id: 1}}), 6464401);
}());
