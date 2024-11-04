/**
 * Basic test for resharding for clustered collections.
 * @tags: [
 *   featureFlagMoveCollection,
 *   assumes_balancer_off,
 * ]
 */

import {ReshardCollectionCmdTest} from "jstests/sharding/libs/reshard_collection_util.js";
import {createChunks, getShardNames} from "jstests/sharding/libs/sharding_util.js";

// This test requires at least two shards.
const shardNames = getShardNames(db);
if (shardNames.length < 2) {
    jsTestLog("This test requires at least two shards.");
    quit();
}

const dbName = db.getName();
const collName = jsTestName();
const ns = `${dbName}.${collName}`;
const coll = db.getCollection(collName);
const mongos = db.getMongo();
const shardKey = {
    oldKey: 1
};
const numDocs = 4000;

jsTestLog("Setting up the clustered collection.");
db.createCollection(
    collName, {clusteredIndex: {"key": shardKey, "unique": true, "name": "clustered collection"}});
assert.commandWorked(db.adminCommand({shardCollection: ns, key: shardKey}));

let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; i++) {
    bulk.insert({oldKey: i});
}
bulk.execute();
assert.eq(numDocs, coll.countDocuments({}));

jsTestLog("Resharding the clustered collection.");
const reshardCmdTest = new ReshardCollectionCmdTest({
    mongos,
    dbName,
    collName,
    numInitialDocs: 0,
    skipDirectShardChecks: true,
    skipCollectionSetup: true,
});

// Reshard to the same shard key.
let newChunks = createChunks(shardNames, 'shardKey', 0, numDocs);
newChunks.forEach((_, idx) => {
    newChunks[idx]["recipientShardId"] = newChunks[idx]["shard"];
    delete newChunks[idx]["shard"];
});
reshardCmdTest.assertReshardCollOkWithPreset({reshardCollection: ns, key: {shardKey: 1}},
                                             newChunks);
assert.eq(numDocs, coll.countDocuments({}));

// Reshard to a different shard key.
newChunks = createChunks(shardNames, 'newShardKey', 0, numDocs);
newChunks.forEach((_, idx) => {
    newChunks[idx]["recipientShardId"] = newChunks[idx]["shard"];
    delete newChunks[idx]["shard"];
});

reshardCmdTest.assertReshardCollOkWithPreset({reshardCollection: ns, key: {newShardKey: 1}},
                                             newChunks);
assert.eq(numDocs, coll.countDocuments({}));

coll.drop();
