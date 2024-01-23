import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

function collectionExists(shard, dbName, collName) {
    return Array.contains(shard.getDB(dbName).getCollectionNames(), collName);
}

var st = new ShardingTest({mongos: 1, shards: 2});

var mongos = st.s0;
var shard0 = st.shard0;
var shard1 = st.shard1;
var config = st.config;

const dbName = 'test_db';
const coll1Name = 'test_coll_1';
const coll2Name = 'test_coll_2';
const coll3Name = 'test_coll_3';
const coll4Name = 'test_coll_4';
const coll1NS = dbName + '.' + coll1Name;
const coll2NS = dbName + '.' + coll2Name;
const coll3NS = dbName + '.' + coll3Name;
const coll4NS = dbName + '.' + coll4Name;

const isMultiversion =
    jsTest.options().shardMixedBinVersions || jsTest.options().useRandomBinVersionsWithinReplicaSet;
const ffTrackUnsharded = !isMultiversion &&
    FeatureFlagUtil.isEnabled(st.configRS.getPrimary(),
                              "TrackUnshardedCollectionsOnShardingCatalog");

assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: shard0.shardName}));
assert.commandWorked(mongos.getCollection(coll1NS).insert({name: 'Tom'}));
assert.commandWorked(mongos.getCollection(coll1NS).insert({name: 'Dick'}));
assert.commandWorked(mongos.getCollection(coll2NS).insert({name: 'Harry'}));
if (ffTrackUnsharded) {
    assert.commandWorked(
        mongos.getDB(dbName).runCommand({createUnsplittableCollection: coll3Name}));
    assert.commandWorked(mongos.getCollection(coll3NS).insert({name: 'Peter'}));

    assert.commandWorked(mongos.getDB(dbName).runCommand(
        {createUnsplittableCollection: coll4Name, dataShard: shard1.shardName}));
    assert.commandWorked(mongos.getCollection(coll4NS).insert({name: 'Jack'}));
}

assert.commandWorked(st.s.adminCommand({shardCollection: coll2NS, key: {_id: 1}}));

jsTest.log('Test preconditions');
{
    // Fail with internal databases.
    assert.commandFailed(mongos.adminCommand({movePrimary: 'config', to: shard1.shardName}));
    assert.commandFailed(mongos.adminCommand({movePrimary: 'admin', to: shard1.shardName}));
    assert.commandFailed(mongos.adminCommand({movePrimary: 'local', to: shard1.shardName}));

    // Fail with invalid database names.
    assert.commandFailed(mongos.adminCommand({movePrimary: '', to: shard1.shardName}));
    assert.commandFailed(mongos.adminCommand({movePrimary: 'a.b', to: shard1.shardName}));

    // Fail against a non-admin database.
    assert.commandFailedWithCode(
        mongos.getDB('test').runCommand({movePrimary: dbName, to: shard1.shardName}),
        ErrorCodes.Unauthorized);

    // Fail if the destination shard is invalid or does not exist.
    assert.commandFailed(mongos.adminCommand({movePrimary: dbName}));
    assert.commandFailed(mongos.adminCommand({movePrimary: dbName, to: ''}));
    assert.commandFailed(mongos.adminCommand({movePrimary: dbName, to: 'Unknown'}));

    // Succeed if the destination shard is already the primary for the given database.
    assert.commandWorked(mongos.adminCommand({movePrimary: dbName, to: shard0.shardName}));
}

jsTest.log('Test that unsharded and unsplittable collections are moved');
{
    {
        // Expected documents placement before moving primary to shard1:
        //   * shard0: 2 docs (3 with featureFlagTrackUnshardedCollectionsOnShardingCatalog)
        //     1: { name : 'Tom'   }
        //     2: { name : 'Dick'  }
        //     3: { name : 'Harry' }
        //     4: { name : 'Peter' } (if featureFlagTrackUnshardedCollectionsOnShardingCatalog is
        //     enabled)
        //   * shard1: 0 docs (1 with featureFlagTrackUnshardedCollectionsOnShardingCatalog enabled)
        //     1: { name : 'Jack'  } (if featureFlagTrackUnshardedCollectionsOnShardingCatalog is
        //     enabled)

        // The unsharded collections' (1&3) documents are on shard0.
        assert.eq(2, shard0.getCollection(coll1NS).find().itcount());
        assert.eq(0, shard1.getCollection(coll1NS).find().itcount());
        if (ffTrackUnsharded) {
            assert.eq(1, shard0.getCollection(coll3NS).find().itcount());
            assert.eq(0, shard1.getCollection(coll3NS).find().itcount());
        }

        // The sharded collection's documents are on shard0.
        assert.eq(1, shard0.getCollection(coll2NS).find().itcount());
        assert.eq(0, shard1.getCollection(coll2NS).find().itcount());

        if (ffTrackUnsharded) {
            // Unsharded collection 4's documents are on shard1.
            assert.eq(1, shard1.getCollection(coll4NS).find().itcount());
            assert.eq(0, shard0.getCollection(coll4NS).find().itcount());
        }
    }

    assert.commandWorked(mongos.adminCommand({movePrimary: dbName, to: shard1.shardName}));

    {
        // Expected documents placement after moving primary to shard1:
        //   * shard0: 1 doc
        //     1: { name : 'Harry' }
        //   * shard1: 2 docs (4 with featureFlagTrackUnshardedCollectionsOnShardingCatalog enabled)
        //     1: { name : 'Tom'   }
        //     2: { name : 'Dick'  }
        //     3: { name : 'Peter' } (if featureFlagTrackUnshardedCollectionsOnShardingCatalog is
        //     enabled)
        //     4: { name : 'Jack'  } (if featureFlagTrackUnshardedCollectionsOnShardingCatalog is
        //     enabled)

        // The unsharded collections' (1&3&4) documents are now on shard1.
        assert.eq(0, shard0.getCollection(coll1NS).find().itcount());
        assert.eq(2, shard1.getCollection(coll1NS).find().itcount());
        if (ffTrackUnsharded) {
            assert.eq(0, shard0.getCollection(coll3NS).find().itcount());
            assert.eq(1, shard1.getCollection(coll3NS).find().itcount());

            assert.eq(0, shard0.getCollection(coll4NS).find().itcount());
            assert.eq(1, shard1.getCollection(coll4NS).find().itcount());
        }

        // The sharded collection's documents are all on shard0.
        assert.eq(1, shard0.getCollection(coll2NS).find().itcount());
        assert.eq(0, shard1.getCollection(coll2NS).find().itcount());
    }

    assert.commandWorked(mongos.adminCommand({movePrimary: dbName, to: shard0.shardName}));

    {
        // Expected documents placement after moving primary back to shard0:
        //   * shard0: 3 docs (5 with featureFlagTrackUnshardedCollectionsOnShardingCatalog enabled)
        //     1: { name : 'Tom'   }
        //     2: { name : 'Dick'  }
        //     3: { name : 'Harry' }
        //     4: { name : 'Peter' } (only with
        //     featureFlagTrackUnshardedCollectionsOnShardingCatalog enabled)
        //     5: { name : 'Jack'  }
        //     (only with featureFlagTrackUnshardedCollectionsOnShardingCatalog enabled)
        //   * shard1: 0 docs

        // The unsharded collections' documents are on shard0.
        assert.eq(2, shard0.getCollection(coll1NS).find().itcount());
        assert.eq(0, shard1.getCollection(coll1NS).find().itcount());
        if (ffTrackUnsharded) {
            assert.eq(1, shard0.getCollection(coll3NS).find().itcount());
            assert.eq(0, shard1.getCollection(coll3NS).find().itcount());

            assert.eq(1, shard0.getCollection(coll4NS).find().itcount());
            assert.eq(0, shard1.getCollection(coll4NS).find().itcount());
        }

        // The sharded collection's documents are on shard0.
        assert.eq(1, shard0.getCollection(coll2NS).find().itcount());
        assert.eq(0, shard1.getCollection(coll2NS).find().itcount());
    }
}

jsTest.log('Test that orphaned documents on recipient causes the operation to fail');
{
    // Insert an orphaned document on shard1.
    assert.commandWorked(shard1.getCollection(coll1NS).insertOne({name: 'Emma'}));

    // The documents are on both the shards.
    assert.eq(2, shard0.getCollection(coll1NS).find().itcount());
    assert.eq(1, shard1.getCollection(coll1NS).find().itcount());

    assert.commandFailedWithCode(mongos.adminCommand({movePrimary: dbName, to: shard1.shardName}),
                                 ErrorCodes.NamespaceExists);

    // The documents are on both the shards.
    assert.eq(2, shard0.getCollection(coll1NS).find().itcount());
    assert.eq(1, shard1.getCollection(coll1NS).find().itcount());

    // Remove the orphaned document on shard1 leaving an empty collection.
    assert.commandWorked(shard1.getCollection(coll1NS).remove({name: 'Emma'}));
    assert.eq(0, shard1.getCollection(coll1NS).find().itcount());

    assert.commandFailedWithCode(mongos.adminCommand({movePrimary: dbName, to: shard1.shardName}),
                                 ErrorCodes.NamespaceExists);

    // Drop the orphaned collection on shard1.
    shard1.getCollection(coll1NS).drop();
}

jsTest.log('Test that metadata has changed');
{
    //  The current primary shard is shard1.
    const previousMetadata = config.databases.findOne({_id: dbName});
    assert.eq(shard0.shardName, previousMetadata.primary);

    assert.commandWorked(mongos.adminCommand({movePrimary: dbName, to: shard1.shardName}));

    // The new primary shard is shard0.
    const nextMetadata = config.databases.findOne({_id: dbName});
    assert.eq(shard1.shardName, nextMetadata.primary);

    // UUID has not changed, but timestamp and lastMod have been bumped.
    assert.eq(previousMetadata.version.uuid, nextMetadata.version.uuid);
    assert.eq(-1, timestampCmp(previousMetadata.version.timestamp, nextMetadata.version.timestamp));
    assert.eq(previousMetadata.version.lastMod + 1, nextMetadata.version.lastMod);
}

st.stop();
