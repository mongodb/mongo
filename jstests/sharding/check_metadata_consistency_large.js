/*
 * Tests to validate the correct behaviour of checkMetadataConsistency command with a lot of
 * inconsistencies.
 *
 * @tags: [
 *    featureFlagCheckMetadataConsistency,
 *    requires_fcv_71,
 *    resource_intensive,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/sharding/libs/create_sharded_collection_util.js");

// Configure initial sharding cluster
const st = new ShardingTest({});
const mongos = st.s;

const dbName = "testCheckMetadataConsistencyDB";
var dbCounter = 0;

function getNewDb() {
    return mongos.getDB(dbName + dbCounter++);
}

(function testManyInconsistencies() {
    // Introduce a misplaced inconsistency
    const db = getNewDb();
    assert.commandWorked(
        mongos.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.shard1.getDB(db.getName()).coll.insert({_id: 'foo'}));

    const kFakeInconsistenciesPerShard = 1000;
    const data = {numInconsistencies: NumberInt(kFakeInconsistenciesPerShard)};
    const fp1 = configureFailPoint(st.shard0, 'insertFakeInconsistencies', data);
    const fp2 = configureFailPoint(st.shard1, 'insertFakeInconsistencies', data);

    // If catalog shard is enabled, there will be introduced inconsistencies in shard0, shard1 and
    // config. Otherwise, only shard0 and shard1.
    const kExpectedInconsistencies = TestData.configShard ? 3 * kFakeInconsistenciesPerShard + 1
                                                          : 2 * kFakeInconsistenciesPerShard + 1;

    let inconsistencies = db.checkMetadataConsistency().toArray();
    assert.eq(kExpectedInconsistencies, inconsistencies.length, tojson(inconsistencies));

    // Clean up the database to pass the hooks that detect inconsistencies
    fp1.off();
    fp2.off();
    db.dropDatabase();
    inconsistencies = mongos.getDB("admin").checkMetadataConsistency().toArray();
    assert.eq(0, inconsistencies.length, tojson(inconsistencies));
})();

(function testMissingManyIndexes() {
    const db = getNewDb();
    const checkOptions = {'checkIndexes': 1};
    const kIndexes = 60;

    assert.commandWorked(st.s.adminCommand({enableSharding: db.getName()}));
    st.ensurePrimaryShard(db.getName(), st.shard0.shardName);
    CreateShardedCollectionUtil.shardCollectionWithChunks(db.coll, {x: 1}, [
        {min: {x: MinKey}, max: {x: 1}, shard: st.shard0.shardName},
        {min: {x: 1}, max: {x: MaxKey}, shard: st.shard1.shardName},
    ]);

    const shard0Coll = st.shard0.getDB(db.getName()).coll;
    const shard1Coll = st.shard1.getDB(db.getName()).coll;

    const shard0Indexes = Array.from({length: kIndexes}, (_, i) => ({['index0' + i]: 1}));
    const shard1Indexes = Array.from({length: kIndexes}, (_, i) => ({['index1' + i]: 1}));
    assert.commandWorked(shard0Coll.createIndexes(shard0Indexes));
    assert.commandWorked(shard1Coll.createIndexes(shard1Indexes));

    // Check that the number of inconsistencies is correct
    let inconsistencies = db.checkMetadataConsistency(checkOptions).toArray();
    assert.eq(kIndexes * 2, inconsistencies.length, tojson(inconsistencies));
    inconsistencies.forEach(inconsistency => {
        assert.eq("InconsistentIndex", inconsistency.type, tojson(inconsistency));
    });

    // Clean up the database to pass the hooks that detect inconsistencies
    assert.commandWorked(db.coll.dropIndexes());
    inconsistencies = db.checkMetadataConsistency({'checkIndexes': 1}).toArray();
    assert.eq(0, inconsistencies.length, tojson(inconsistencies));
})();

st.stop();
})();
