/**
 * Test that chunk operations preserve collection UUID in config.chunks documents
 *
 * @tags: [assumes_balancer_off]
 */

const dbName = db.getName();
const collName = jsTestName();
const coll = db.getCollection(collName);
const ns = dbName + "." + collName;
const config = db.getSiblingDB('config');
const shardNames = db.adminCommand({listShards: 1}).shards.map(shard => shard._id);

if (shardNames.length < 2) {
    print(jsTestName() + " will not run; at least 2 shards are required.");
    quit();
}

function allChunksWithUUID() {
    const matchChunksWithoutUUID = {'uuid': null};
    assert.eq(0,
              config.chunks.countDocuments(matchChunksWithoutUUID),
              "Found chunks with wrong UUID " +
                  tojson(config.chunks.find(matchChunksWithoutUUID).toArray()));
}

print(jsTestName() + " is running on " + shardNames.length + " shards.");
assert.commandWorked(db.adminCommand({enableSharding: dbName}));

assert.commandWorked(db.adminCommand({shardCollection: ns, key: {x: 1}}));

assert.commandWorked(db.adminCommand({split: ns, middle: {x: -10}}));
allChunksWithUUID();

assert.commandWorked(db.adminCommand({split: ns, middle: {x: 10}}));
allChunksWithUUID();

assert.commandWorked(db.adminCommand({moveChunk: ns, find: {x: -100}, to: shardNames[0]}));
allChunksWithUUID();

assert.commandWorked(db.adminCommand({moveChunk: ns, find: {x: 0}, to: shardNames[1]}));
allChunksWithUUID();
assert.commandWorked(db.adminCommand({moveChunk: ns, find: {x: 1000}, to: shardNames[1]}));
allChunksWithUUID();

assert.commandWorked(db.adminCommand({split: ns, middle: {x: -500}}));
allChunksWithUUID();
assert.commandWorked(db.adminCommand({mergeChunks: ns, bounds: [{x: MinKey}, {x: -10}]}));
allChunksWithUUID();

assert.commandWorked(db.adminCommand({moveChunk: ns, find: {x: -100}, to: shardNames[1]}));
allChunksWithUUID();
assert.commandWorked(db.adminCommand({moveChunk: ns, find: {x: -100}, to: shardNames[0]}));
allChunksWithUUID();

coll.drop();
