(function() {
'use strict';

load('jstests/libs/feature_flag_util.js');

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
const coll1NS = dbName + '.' + coll1Name;
const coll2NS = dbName + '.' + coll2Name;

assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: shard0.shardName}));
assert.commandWorked(mongos.getCollection(coll1NS).insert({name: 'Tom'}));
assert.commandWorked(mongos.getCollection(coll1NS).insert({name: 'Dick'}));
assert.commandWorked(mongos.getCollection(coll2NS).insert({name: 'Harry'}));

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

jsTest.log('Test that only unsharded collections are moved');
{
    {
        // Expected documents placement before moving primary to shard1:
        //   * shard0: 3 docs
        //     1: { name : 'Tom'   }
        //     2: { name : 'Dick'  }
        //     3: { name : 'Harry' }
        //   * shard1: 0 docs

        // The sharded collection's documents are on shard0.
        assert.eq(2, shard0.getCollection(coll1NS).find().itcount());
        assert.eq(0, shard1.getCollection(coll1NS).find().itcount());

        // The unsharded collection's documents are on shard0.
        assert.eq(1, shard0.getCollection(coll2NS).find().itcount());
        assert.eq(0, shard1.getCollection(coll2NS).find().itcount());
    }

    assert.commandWorked(mongos.adminCommand({movePrimary: dbName, to: shard1.shardName}));

    {
        // Expected documents placement after moving primary to shard1:
        //   * shard0: 1 doc
        //     1: { name : 'Harry' }
        //   * shard1: 2 docs
        //     1: { name : 'Tom'   }
        //     2: { name : 'Dick'  }

        // The sharded collection's documents are now on shard1.
        assert.eq(0, shard0.getCollection(coll1NS).find().itcount());
        assert.eq(2, shard1.getCollection(coll1NS).find().itcount());

        // The unsharded collection's documents are still on shard0.
        assert.eq(1, shard0.getCollection(coll2NS).find().itcount());
        assert.eq(0, shard1.getCollection(coll2NS).find().itcount());
    }

    assert.commandWorked(mongos.adminCommand({movePrimary: dbName, to: shard0.shardName}));

    {
        // Expected documents placement after moving primary back to shard0:
        //   * shard0: 3 docs
        //     1: { name : 'Tom'   }
        //     2: { name : 'Dick'  }
        //     3: { name : 'Harry' }
        //   * shard1: 0 docs

        // The sharded collection's documents are on shard0.
        assert.eq(2, shard0.getCollection(coll1NS).find().itcount());
        assert.eq(0, shard1.getCollection(coll1NS).find().itcount());

        // The unsharded collection's documents are on shard0.
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

    const expectDropOnFailure =
        FeatureFlagUtil.isPresentAndEnabled(config.admin, 'OnlineMovePrimaryLifecycle');

    if (expectDropOnFailure) {
        // The orphaned collection on shard1 should have been dropped due to the previous failure.
        assert.eq(2, shard0.getCollection(coll1NS).find().itcount());
        assert(!collectionExists(shard1, dbName, coll1Name));

        // Create another empty collection.
        shard1.getDB(dbName).createCollection(coll1Name);
    } else {
        // The documents are on both the shards.
        assert.eq(2, shard0.getCollection(coll1NS).find().itcount());
        assert.eq(1, shard1.getCollection(coll1NS).find().itcount());

        // Remove the orphaned document on shard1 leaving an empty collection.
        assert.commandWorked(shard1.getCollection(coll1NS).remove({name: 'Emma'}));
        assert.eq(0, shard1.getCollection(coll1NS).find().itcount());
    }

    assert.commandFailedWithCode(mongos.adminCommand({movePrimary: dbName, to: shard1.shardName}),
                                 ErrorCodes.NamespaceExists);

    if (expectDropOnFailure) {
        // The orphaned collection on shard1 should have been dropped due to the previous failure.
        assert(!collectionExists(shard1, dbName, coll1Name));
    } else {
        // Drop the orphaned collection on shard1.
        shard1.getCollection(coll1NS).drop();
    }
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

    // The identifiers have not changed, but the version (lastMod) has been bumped.
    assert.eq(previousMetadata.version.uuid, nextMetadata.version.uuid);
    assert.eq(previousMetadata.version.timestamp, nextMetadata.version.timestamp);
    assert.eq(previousMetadata.version.lastMod + 1, nextMetadata.version.lastMod);
}

st.stop();
})();
