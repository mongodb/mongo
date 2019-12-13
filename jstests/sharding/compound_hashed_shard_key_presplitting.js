/**
 * Tests the pre-splitting behaviour of compound hashed shard key, for both the case where the
 * prefix field is hashed, and where the hashed field is not the prefix.
 *
 * @tags: [requires_fcv_44, multiversion_incompatible]
 */
(function() {
'use strict';
const st = new ShardingTest({name: jsTestName(), shards: 2});
const dbname = "test";
const mongos = st.s0;
const db = st.getDB(dbname);
db.adminCommand({enablesharding: dbname});
st.ensurePrimaryShard('test', st.shard1.shardName);

/**
 * Test that 'shardCollection' command works when there is existing data in collection and does not
 * do pre-splitting.
 */
[{a: "hashed", rangeField1: 1, rangeField2: 1}, {rangeField1: 1, a: "hashed", rangeField2: 1}]
    .forEach(function(shardKey) {
        db.collWithData.drop();
        db.collWithData.insert({a: 1});
        db.collWithData.createIndex(shardKey);

        // Verify that command fails when 'numInitialChunks' is specified.
        assert.commandFailedWithCode(db.adminCommand({
            shardcollection: db.collWithData.getFullName(),
            key: shardKey,
            numInitialChunks: 500
        }),
                                     ErrorCodes.InvalidOptions);

        assert.commandWorked(
            db.adminCommand({shardcollection: db.collWithData.getFullName(), key: shardKey}));
        assert.eq(st.config.chunks.count({ns: db.collWithData.getFullName()}),
                  1,
                  "sharding non-empty collection should not pre-split");
    });

/**
 * Validates that the chunks ranges have all the shard key fields and each shard has expected number
 * of chunks.
 */
function checkValidChunks(coll, shardKey, expectedChunksOnShard0, expectedChunksOnShard1) {
    const chunks = st.config.chunks.find({"ns": coll.getFullName()}).toArray();
    let shardCountsMap = {[st.shard0.shardName]: 0, [st.shard1.shardName]: 0};
    for (let chunk of chunks) {
        shardCountsMap[chunk.shard]++;

        const assertHasAllShardKeyFields = function(obj) {
            assert.eq(Object.keys(shardKey).length, Object.keys(obj).length, tojson(obj));
            for (let key in obj) {
                assert(key in shardKey, tojson(obj));
            }
        };
        assertHasAllShardKeyFields(chunk.min);
        assertHasAllShardKeyFields(chunk.max);
    }
    assert.eq(expectedChunksOnShard0,
              shardCountsMap[st.shard0.shardName],
              'Count mismatch on shard0: ' + tojson(chunks));
    assert.eq(expectedChunksOnShard1,
              shardCountsMap[st.shard1.shardName],
              'Count mismatch on shard1: ' + tojson(chunks));
}

function runTest(shardKey) {
    // Supported: Hashed sharding + numInitialChunks + empty collection.
    // Expected: Even chunk distribution.
    assert.commandWorked(db.hashedCollEmpty.createIndex(shardKey));
    let coll = db.hashedCollEmpty;
    assert.commandWorked(mongos.adminCommand(
        {shardCollection: coll.getFullName(), key: shardKey, numInitialChunks: 6}));
    checkValidChunks(coll, shardKey, 3, 3);

    // Supported: Hashed sharding + numInitialChunks + non-existent collection.
    // Expected: Even chunk distribution.
    coll = db.hashedCollNonExistent;
    assert.commandWorked(mongos.adminCommand(
        {shardCollection: coll.getFullName(), key: shardKey, numInitialChunks: 6}));
    checkValidChunks(coll, shardKey, 3, 3);

    // Default pre-splitting.
    coll = db.hashedDefaultPreSplit;
    assert.commandWorked(mongos.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));
    checkValidChunks(coll, shardKey, 2, 2);
}

/**
 * Hashed field is a prefix.
 */
runTest({aKey: "hashed", rangeField1: 1, rangeField2: 1});

/**
 * When hashed field is not prefix.
 * TODO SERVER-43917: Add tests when pre-splitting is enabled for non-prefixes.
 */
db.coll.drop();
let shardKey = {rangeField1: 1, a: "hashed", rangeField2: 1};
assert.commandFailedWithCode(
    db.adminCommand({shardcollection: db.coll.getFullName(), key: shardKey, numInitialChunks: 500}),
    ErrorCodes.InvalidOptions);

st.stop();
})();