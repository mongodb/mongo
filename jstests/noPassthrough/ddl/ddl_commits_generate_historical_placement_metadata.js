/**
 * Verifies that successful commits of Sharding DDL operations generate the expected metadata
 * (config.placementHistory documents on the config server and op entries on data bearing shards),
 * as defined by the protocol that supports V2 change stream readers.
 *
 * @tags: [
 *   featureFlagChangeStreamPreciseShardTargeting,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let configDB = null;

let st = new ShardingTest({
    shards: 3,
    chunkSize: 1,
    configOptions:
        {setParameter:
             {reshardingCriticalSectionTimeoutMillis: 24 * 60 * 60 * 1000, /* 1 day */}}
});

configDB = st.s.getDB('config');
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;
const shard2 = st.shard2.shardName;

const shards = [shard0, shard1, shard2];

function randomShard() {
    const randomIdx = Math.floor(Math.random() * shards.length);
    return shards[randomIdx];
}

const replicaSetByShardId = {
    [shard0]: st.rs0,
    [shard1]: st.rs1,
    [shard2]: st.rs2,
};

// Creates a database + a sharded collection with a single chunk; the DB primary shard and the
// location of the chunk are randomly chosen.
function setupDbWithShardedCollection(
    dbName, collName, primaryShard = null, dataBearingShard = null) {
    const nss = dbName + '.' + collName;

    if (!primaryShard) {
        primaryShard = randomShard();
    }

    if (!dataBearingShard) {
        dataBearingShard = randomShard();
    }

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShard}));
    assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {_id: 1}}));

    // Ensure that each shard of the cluster owned the chunk at some point in time (so that it also
    // inserted metadata for the parent collection in its local catalog). This ensures that once the
    // collection is dropped or renamed, each shard will emit the expected commit op entries.
    for (let shard of shards) {
        if (shard !== primaryShard && shard !== dataBearingShard) {
            assert.commandWorked(
                st.s.adminCommand({moveChunk: nss, find: {_id: MinKey}, to: shard}));
        }
    }

    assert.commandWorked(
        st.s.adminCommand({moveChunk: nss, find: {_id: MinKey}, to: dataBearingShard}));
    return {primaryShard, dataBearingShard};
}

function getInfoFromConfigDatabases(dbName) {
    const configDBsQueryResults = configDB.databases.find({_id: dbName}).toArray();
    if (configDBsQueryResults.length === 0) {
        return null;
    }

    assert.eq(1, configDBsQueryResults.length);
    return configDBsQueryResults[0];
}

function getInfoFromConfigCollections(fullCollName) {
    const configCollsQueryResults = configDB.collections.find({_id: fullCollName}).toArray();
    if (configCollsQueryResults.length === 0) {
        return null;
    }

    assert.eq(configCollsQueryResults.length, 1);
    return configCollsQueryResults[0];
}

function getLatestPlacementEntriesFor(namespace, numEntries) {
    return configDB.placementHistory.find({nss: namespace})
        .sort({timestamp: -1})
        .limit(numEntries)
        .toArray();
}

function getLatestPlacementInfoFor(namespace) {
    const placementQueryResults = getLatestPlacementEntriesFor(namespace, 1);
    if (placementQueryResults.length === 0) {
        return null;
    }

    assert.eq(placementQueryResults.length, 1);
    return placementQueryResults[0];
}

function getValidatedPlacementInfoForDB(dbName) {
    const configDBInfo = getInfoFromConfigDatabases(dbName);
    const dbPlacementInfo = getLatestPlacementInfoFor(dbName);
    assert.neq(null, configDBInfo);
    assert.neq(null, dbPlacementInfo);
    // Verify that the placementHistory document matches the related content stored in
    // config.databases.
    assert.sameMembers([configDBInfo.primary], dbPlacementInfo.shards);

    assert(timestampCmp(configDBInfo.version.timestamp, dbPlacementInfo.timestamp) === 0);

    // No UUID field for DB namespaces
    assert.eq(undefined, dbPlacementInfo.uuid);
    return dbPlacementInfo;
}

function getValidatedPlacementInfoForDroppedDb(dbName) {
    const dbPlacementAfterDrop = getLatestPlacementInfoFor(dbName);
    assert.neq(null, dbPlacementAfterDrop);
    assert.eq(0, dbPlacementAfterDrop.shards.length);
    assert.eq(undefined, dbPlacementAfterDrop.uuid);
    return dbPlacementAfterDrop;
}

function getValidatedPlacementInfoForCollection(
    dbName, collName, expectedShardList, isInitialPlacement = false) {
    const fullName = dbName + '.' + collName;
    const configCollInfo = getInfoFromConfigCollections(fullName);
    assert.neq(null, configCollInfo);

    // Verify that there is consistent placement info on the sharded collection and its parent DB.
    const collPlacementInfo = getLatestPlacementInfoFor(fullName);
    assert.neq(null, collPlacementInfo);
    const dbPlacementInfo = getValidatedPlacementInfoForDB(dbName);
    assert(timestampCmp(dbPlacementInfo.timestamp, collPlacementInfo.timestamp) < 0);

    assert.eq(configCollInfo.uuid, collPlacementInfo.uuid);
    if (isInitialPlacement) {
        assert(timestampCmp(configCollInfo.timestamp, collPlacementInfo.timestamp) === 0);
    } else {
        assert(timestampCmp(configCollInfo.timestamp, collPlacementInfo.timestamp) <= 0);
    }

    assert.sameMembers(expectedShardList, collPlacementInfo.shards);
    return collPlacementInfo;
}

function getValidatedPlacementInfoForDroppedColl(collNss, collUUID) {
    const collPlacementAfterDrop = getLatestPlacementInfoFor(collNss);
    assert.neq(null, collPlacementAfterDrop);
    assert.eq(0, collPlacementAfterDrop.shards.length);
    assert.eq(collUUID, collPlacementAfterDrop.uuid);
    return collPlacementAfterDrop;
}

function makeDropCollectionEntryTemplate(dbName, collName, numDroppedDocs = 0) {
    return {op: 'c', ns: `${dbName}.$cmd`, o: {drop: collName}, o2: {numRecords: numDroppedDocs}};
}

function makeCreateCollectionEntryTemplate(dbName, collName) {
    return {
        op: 'c',
        ns: `${dbName}.$cmd`,
        o: {create: collName, idIndex: {v: 2, key: {_id: 1}, name: '_id_'}}
    };
}

function makeCreateIndexesEntryTemplate(dbName, collName, indexSpec) {
    const indexName = Object.keys(indexSpec).reduce((acc, key) => {
        return acc + key + '_' + indexSpec[key];
    }, '');
    return {
        op: 'c',
        ns: `${dbName}.$cmd`,
        o: {createIndexes: collName, spec: {v: 2, key: indexSpec, name: indexName}},

    };
}

function makeShardCollectionEntryTemplate(dbName, collName, shardKeyObj) {
    const nss = dbName + '.' + collName;
    return {
        'op': 'n',
        'ns': nss,
        'o': {'msg': {'shardCollection': nss}},
        'o2': {
            'shardCollection': nss,
            'shardKey': shardKeyObj,
            'unique': false,
            'presplitHashedZones': false,
            'capped': false
        }
    };
}

function makeRenameCollectionEntryTemplate(
    commonDbName, sourceCollName, targetCollName, targetUUID = null) {
    let entryTemplate = {
        op: 'c',
        ns: `${commonDbName}.$cmd`,
        o: {
            renameCollection: `${commonDbName}.${sourceCollName}`,
            to: `${commonDbName}.${targetCollName}`,
            stayTemp: false
        }
    };

    if (targetUUID) {
        entryTemplate.o.dropTarget = targetUUID;
        entryTemplate.o2 = {numRecords: 0};
    }

    return entryTemplate;
}

function makeDropDatabaseEntryTemplate(dbName) {
    return {op: 'c', ns: `${dbName}.$cmd`, o: {dropDatabase: 1}};
}

function makeMoveChunkEntryTemplate(nss, donor, recipient, noMoreChunksOnDonor) {
    return {
        op: 'n',
        ns: nss,
        o: {msg: {moveChunk: nss}},
        o2: {
            moveChunk: nss,
            donor: donor,
            recipient: recipient,
            allCollectionChunksMigratedFromDonor: noMoreChunksOnDonor
        }
    };
}

function makeChunkOnNewShardEntryTemplate(nss, donor, recipient) {
    return {
        op: 'n',
        ns: nss,
        o: {
            msg: `Migrating chunk from shard ${donor} to shard ${
                recipient} with no chunks for this collection`
        },
        o2: {migrateChunkToNewShard: nss, fromShardId: donor, toShardId: recipient}
    };
}

function makeOpEntryOnEmptiedDonor(nss, donor) {
    return {
        op: 'n',
        ns: nss,
        o: {msg: `Migrate the last chunk for ${nss} off shard ${donor}`},
        o2: {migrateLastChunkFromShard: nss, shardId: donor}
    };
}

function makePlacementChangedEntryTemplate(commitTime, dbName, collName = null) {
    const nss = collName ? dbName + '.' + collName : dbName;
    const o2Value = {namespacePlacementChanged: 1, ns: {db: dbName}, committedAt: commitTime};
    if (collName) {
        o2Value.ns.coll = collName;
    }

    return {op: 'n', ns: nss, o: {msg: {namespacePlacementChanged: nss}}, o2: o2Value};
};

// Verifies that the expected un/ordered sequence of op entries is generated on each shard
// at the top of its op log.
// Each comparison is performed over a subset of op entry fields to remove dependencies on
// timestamps and active FCV version.
// The function also returns the raw entries retrieved on each shard (in increasing 'ts' order).
function verifyCommitOpEntriesOnShards(expectedOpEntryTemplates, shards, orderStrict = true) {
    const namespaces = [...new Set(expectedOpEntryTemplates.map(t => t.ns))];
    const foundOpEntriesByShard = {};
    for (const shard of shards) {
        const foundOpEntries = replicaSetByShardId[shard]
                                   .getPrimary()
                                   .getCollection('local.oplog.rs')
                                   .find({
                                       ns: {$in: namespaces},
                                       op: {$in: ['c', 'n']},
                                       // Discard entries related to
                                       // authoritative db metadata management.
                                       'o.dropDatabaseMetadata': {$exists: false},
                                       'o.createDatabaseMetadata': {$exists: false},
                                   })
                                   .sort({ts: -1})
                                   .limit(expectedOpEntryTemplates.length)
                                   .toArray()
                                   .reverse();

        // Strip out timing-related entry fields before performing the comparison.
        const redactedOpEntries = foundOpEntries.map((opEntry) => {
            let {ui, ts, t, v, wall, versionContext, fromMigrate, ...strippedOpEntry} = opEntry;
            if (opEntry.o.create || opEntry.o.createIndexes) {
                // Also strip out the 'o2' field, containing references to the ident values for the
                // created collection & index.
                delete strippedOpEntry.o2;
            }
            return strippedOpEntry;
        });

        if (orderStrict) {
            assert.eq(expectedOpEntryTemplates.length, redactedOpEntries.length);
            for (let i = 0; i < foundOpEntries.length; ++i) {
                assert.eq(expectedOpEntryTemplates[i], redactedOpEntries[i]);
            }
        } else {
            assert.sameMembers(expectedOpEntryTemplates, redactedOpEntries);
        }
        foundOpEntriesByShard[shard] = foundOpEntries;
    }

    return foundOpEntriesByShard;
}

function testEnableSharding(dbName, primaryShardName) {
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShardName}));
    getValidatedPlacementInfoForDB(dbName);
}

function testShardCollection() {
    {
        jsTest.log(`Testing shardCollection() with implicit database and collection creation`);
        const dbName = 'shardCollectionTestImplicitlyCreatedDB';
        const collName = 'implicitlyCreatedColl';
        const nss = dbName + '.' + collName;
        assert.eq(null, getLatestPlacementInfoFor(dbName));
        assert.eq(null, getLatestPlacementInfoFor(nss));
        // By applying a hashed shard key, the collection is expected to have a chunk on each shard.
        const shardKeySpec = {_id: 'hashed'};
        assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: shardKeySpec}));

        const dbPlacementUponShardCollectionCompleted = getValidatedPlacementInfoForDB(dbName);
        const collPlacementUponShardCollectionCompleted =
            getValidatedPlacementInfoForCollection(dbName, collName, shards, true);
        assert(timestampCmp(dbPlacementUponShardCollectionCompleted.timestamp,
                            collPlacementUponShardCollectionCompleted.timestamp) < 0);

        // Verify that the primary shard emitted the expected events.
        const primaryShard = dbPlacementUponShardCollectionCompleted.shards[0];
        const expectedEntryTemplates = [
            makeCreateCollectionEntryTemplate(dbName, collName),
            makeCreateIndexesEntryTemplate(dbName, collName, shardKeySpec),
            makeShardCollectionEntryTemplate(dbName, collName, shardKeySpec),
            makePlacementChangedEntryTemplate(
                collPlacementUponShardCollectionCompleted.timestamp, dbName, collName)
        ];

        const [createCollEntry, _, shardCollEntry, collPlacementChangedEntry] =
            verifyCommitOpEntriesOnShards(expectedEntryTemplates, [primaryShard])[primaryShard];

        // Commit events are user visible
        assert(!createCollEntry.fromMigrate || createCollEntry.fromMigrate === false);
        assert(!shardCollEntry.fromMigrate || shardCollEntry.fromMigrate === false);
        // The placement change docs and the expected events are genereted with the expected timing.
        assert(timestampCmp(dbPlacementUponShardCollectionCompleted.timestamp, createCollEntry.ts) <
               0);
        assert(timestampCmp(shardCollEntry.ts,
                            collPlacementUponShardCollectionCompleted.timestamp) < 0);
        assert(timestampCmp(collPlacementUponShardCollectionCompleted.timestamp,
                            collPlacementChangedEntry.ts) <= 0);
    }

    {
        jsTest.log('Testing shardCollection() with an existing unsplittable collection');
        const dbName = 'shardCollectionTestUnsplittableDB';
        const collName = 'unsplittableColl';
        const nss = dbName + '.' + collName;
        const primaryShard = shards[0];
        const dataBearingShard = shards[1];
        const shardKeySpec = {x: 1};

        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShard}));

        assert.commandWorked(st.s.getDB(dbName).createCollection(collName));
        assert.commandWorked(st.s.adminCommand({moveCollection: nss, toShard: dataBearingShard}));

        const placementUponMoveCollection = getValidatedPlacementInfoForCollection(
            dbName, collName, [dataBearingShard], true /*isInitialPlacement*/);

        assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: shardKeySpec}));

        const placementUponShardCollection = getValidatedPlacementInfoForCollection(
            dbName, collName, [dataBearingShard], true /*isInitialPlacement*/);
        assert.eq(placementUponMoveCollection.uuid, placementUponShardCollection.uuid);
        assert(timestampCmp(placementUponMoveCollection.timestamp,
                            placementUponShardCollection.timestamp) < 0);

        // Commit events are expected to be emitted by the current data bearing shard...
        const expectedEntryTemplatesOnDataShard = [
            makeShardCollectionEntryTemplate(dbName, collName, shardKeySpec),
            makePlacementChangedEntryTemplate(
                placementUponShardCollection.timestamp, dbName, collName)
        ];

        const [shardCollEntry, collPlacementChangedEntry] = verifyCommitOpEntriesOnShards(
            expectedEntryTemplatesOnDataShard, [dataBearingShard])[dataBearingShard];

        // ... With the expected user visibility and timing
        assert(!shardCollEntry.fromMigrate || shardCollEntry.fromMigrate === false);

        // The placement change docs and the expected events are genereted with the expected timing.
        assert(timestampCmp(shardCollEntry.ts, placementUponShardCollection.timestamp) < 0);
        assert(timestampCmp(placementUponShardCollection.timestamp, collPlacementChangedEntry.ts) <
               0);

        // ... while the primary shard should only emit an entry related to the creation of the
        // shard key index.
        const expectedEntryTemplatesOnPrimaryShard = [
            makeCreateIndexesEntryTemplate(dbName, collName, shardKeySpec),
        ];

        const [createIndexesEntry] = verifyCommitOpEntriesOnShards(
            expectedEntryTemplatesOnPrimaryShard, [primaryShard])[primaryShard];

        assert(!createIndexesEntry.fromMigrate || createIndexesEntry.fromMigrate === false);
    }

    {
        jsTest.log(
            'Testing shardCollection() (with implicit collection creation) against a preallocated set of zones that prevents the primary shard from bearing any data');
        const dbName = 'shardCollectionTestWithZonesDB';
        const collName = 'shardedColl';
        const nss = dbName + '.' + collName;
        const primaryShard = shards[0];
        const shardWithZone = shards[1];
        const shardKeySpec = {x: 1};

        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShard}));

        // Create a zone and assign it to a non-primary shard.
        assert.commandWorked(
            st.s.adminCommand({addShardToZone: shardWithZone, zone: 'zoneForShardedColl'}));
        // Allocate the whole routing table range to the zone, so that the primary shard
        // won't be able own any data.
        assert.commandWorked(st.s.adminCommand({
            updateZoneKeyRange: nss,
            min: {x: MinKey},
            max: {x: MaxKey},
            zone: 'zoneForShardedColl'
        }));

        assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: shardKeySpec}));

        const placementInfo = getValidatedPlacementInfoForCollection(
            dbName, collName, [shardWithZone], true /*isInitialPlacement*/);

        // Despite the zone constraint, the primary shard should still be the one
        // responsible for emitting the expected commit op entries.
        const expectedEntryTemplates = [
            makeCreateCollectionEntryTemplate(dbName, collName),
            makeCreateIndexesEntryTemplate(dbName, collName, shardKeySpec),
            makeShardCollectionEntryTemplate(dbName, collName, shardKeySpec),
            makePlacementChangedEntryTemplate(placementInfo.timestamp, dbName, collName)
        ];

        const [createCollEntry, _, shardCollEntry, collPlacementChangedEntry] =
            verifyCommitOpEntriesOnShards(expectedEntryTemplates, [primaryShard])[primaryShard];

        // Commit events are user visible
        assert(!createCollEntry.fromMigrate || createCollEntry.fromMigrate === false);
        assert(!shardCollEntry.fromMigrate || shardCollEntry.fromMigrate === false);
    }
}

function testMoveChunk(dbName, collName) {
    // Setup - All the data are contained by a single chunk on the primary shard
    const nss = dbName + '.' + collName;
    testEnableSharding(dbName, shard0);
    assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {x: 1}}));
    const collPlacementInfoAtCreationTime =
        getValidatedPlacementInfoForCollection(dbName, collName, [shard0], true);
    const collUUID = collPlacementInfoAtCreationTime.uuid;
    assert.eq(1, configDB.chunks.count({uuid: collUUID}));

    // Create two chunks, then move 1 to shard1 -> the recipient should be present in a new
    // placement entry
    st.s.adminCommand({split: nss, middle: {x: 0}});
    assert.commandWorked(st.s.adminCommand({moveChunk: nss, find: {x: -1}, to: shard1}));
    let placementAfterMigration =
        getValidatedPlacementInfoForCollection(dbName, collName, [shard0, shard1]);
    let migratedChunk = configDB.chunks.findOne({uuid: collUUID, min: {x: MinKey}});
    assert(timestampCmp(placementAfterMigration.timestamp, migratedChunk.history[0].validAfter) ===
           0);
    assert(timestampCmp(placementAfterMigration.timestamp, migratedChunk.onCurrentShardSince) ===
           0);
    // The commit should also be matched by two no-op entries
    const expectedEntriesForNewOwningShard = [
        makeMoveChunkEntryTemplate(nss, shard0, shard1, false /*noMoreChunksOnDonor*/),
        makeChunkOnNewShardEntryTemplate(nss, shard0, shard1)
    ];

    verifyCommitOpEntriesOnShards(
        expectedEntriesForNewOwningShard, [shard0], false /*orderStrict*/);

    // Move out the last chunk from shard0 to shard2 - a new placement entry should appear,
    // where the donor has been removed and the recipient inserted
    assert.commandWorked(st.s.adminCommand({moveChunk: nss, find: {x: 1}, to: shard2}));
    placementAfterMigration =
        getValidatedPlacementInfoForCollection(dbName, collName, [shard1, shard2]);
    migratedChunk = configDB.chunks.findOne({uuid: collUUID, min: {x: 0}});
    assert(timestampCmp(placementAfterMigration.timestamp, migratedChunk.history[0].validAfter) ===
           0);
    assert(timestampCmp(placementAfterMigration.timestamp, migratedChunk.onCurrentShardSince) ===
           0);
    // The commit should also be matched by three no-op entries
    const expectedEntriesForNewOwningShardPlusEmptyDonor = [
        makeMoveChunkEntryTemplate(nss, shard0, shard2, true /*noMoreChunksOnDonor*/),
        makeOpEntryOnEmptiedDonor(nss, shard0),
        makeChunkOnNewShardEntryTemplate(nss, shard0, shard2)
    ];

    verifyCommitOpEntriesOnShards(
        expectedEntriesForNewOwningShardPlusEmptyDonor, [shard0], false /*orderStrict*/);

    // Create a third chunk in shard1, then move it to shard2: since this migration does not
    // alter the subset of shards owning collection data, no new record should be inserted
    const numPlacementEntriesBeforeMigration = configDB.placementHistory.count({nss: nss});
    st.s.adminCommand({split: nss, middle: {x: 10}});
    assert.commandWorked(st.s.adminCommand({moveChunk: nss, find: {x: 10}, to: shard1}));
    const numPlacementEntriesAfterMigration = configDB.placementHistory.count({nss: nss});
    assert.eq(numPlacementEntriesBeforeMigration, numPlacementEntriesAfterMigration);
    // The commit should still be be matched by a single no-op entry.s
    const expectedEntriesForChunkMigrationWithouthPlacementChanges =
        [makeMoveChunkEntryTemplate(nss, shard2, shard1, false /*noMoreChunksOnDonor*/)];

    verifyCommitOpEntriesOnShards(
        expectedEntriesForChunkMigrationWithouthPlacementChanges, [shard2], false /*orderStrict*/);
}

function testMoveRange(dbName, collName) {
    // Setup - All the data are contained by a single chunk on the primary shard
    const nss = dbName + '.' + collName;
    testEnableSharding(dbName, shard0);
    assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {x: 1}}));
    const collPlacementInfoAtCreationTime =
        getValidatedPlacementInfoForCollection(dbName, collName, [shard0], true);
    const collUUID = collPlacementInfoAtCreationTime.uuid;
    assert.eq(1, configDB.chunks.count({uuid: collUUID}));

    // Move half of the existing chunk to shard 1 -> the recipient should be added to the
    // placement data, while the donor should emit two op entries
    assert.commandWorked(
        st.s.adminCommand({moveRange: nss, min: {x: MinKey}, max: {x: 0}, toShard: shard1}));
    let placementAfterMigration =
        getValidatedPlacementInfoForCollection(dbName, collName, [shard0, shard1]);
    let migratedChunk = configDB.chunks.findOne({uuid: collUUID, min: {x: MinKey}});
    assert(timestampCmp(placementAfterMigration.timestamp, migratedChunk.history[0].validAfter) ===
           0);
    assert(timestampCmp(placementAfterMigration.timestamp, migratedChunk.onCurrentShardSince) ===
           0);

    const expectedEntriesForNewOwningShard = [
        makeMoveChunkEntryTemplate(nss, shard0, shard1, false /*noMoreChunksOnDonor*/),
        makeChunkOnNewShardEntryTemplate(nss, shard0, shard1)
    ];
    verifyCommitOpEntriesOnShards(
        expectedEntriesForNewOwningShard, [shard0], false /*orderStrict*/);

    // Move the other half to shard 1 -> shard 0 should be removed from the placement data and
    // generate two op entries.
    assert.commandWorked(
        st.s.adminCommand({moveRange: nss, min: {x: 0}, max: {x: MaxKey}, toShard: shard1}));
    placementAfterMigration = getValidatedPlacementInfoForCollection(dbName, collName, [shard1]);
    migratedChunk = configDB.chunks.findOne({uuid: collUUID, min: {x: 0}});
    assert(timestampCmp(placementAfterMigration.timestamp, migratedChunk.history[0].validAfter) ===
           0);
    assert(timestampCmp(placementAfterMigration.timestamp, migratedChunk.onCurrentShardSince) ===
           0);

    const expectedEntriesEmptyDonor = [
        makeMoveChunkEntryTemplate(nss, shard0, shard1, true /*noMoreChunksOnDonor*/),
        makeOpEntryOnEmptiedDonor(nss, shard0),
    ];
    verifyCommitOpEntriesOnShards(expectedEntriesEmptyDonor, [shard0], false /*orderStrict*/);
}

function testMovePrimary(dbName, fromPrimaryShardName, toPrimaryShardName) {
    // Create the database
    testEnableSharding(dbName, fromPrimaryShardName);

    // Move the primary shard
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: toPrimaryShardName}));

    // Verify that the new primary shard is the one specified in the command.
    const newDbInfo = getValidatedPlacementInfoForDB(dbName);
    assert.sameMembers(newDbInfo.shards, [toPrimaryShardName]);
}

function testDropCollection() {
    const dbName = 'dropCollectionTestDB';
    const collName = 'shardedCollName';
    const nss = dbName + '.' + collName;
    const db = st.s.getDB(dbName);

    // 1) Verify that the drop of a sharded collection generates the expected metadata
    let {primaryShard, dataBearingShard} = setupDbWithShardedCollection(dbName, collName);

    const initialPlacementInfo = getLatestPlacementInfoFor(nss);
    const numHistoryEntriesBeforeFirstDrop = configDB.placementHistory.count({nss: nss});

    assert.commandWorked(db.runCommand({drop: collName}));

    // 1.a) A single config.placementHistory document has been inserted.
    const numHistoryEntriesAfterFirstDrop = configDB.placementHistory.count({nss: nss});
    assert.eq(numHistoryEntriesBeforeFirstDrop + 1, numHistoryEntriesAfterFirstDrop);
    const collPlacementAfterDrop =
        getValidatedPlacementInfoForDroppedColl(nss, initialPlacementInfo.uuid);
    assert(timestampCmp(initialPlacementInfo.timestamp, collPlacementAfterDrop.timestamp) < 0);

    // 1.b) The only data-bearing shard has generated a commit op entry, followed by a
    // namespacePlacementChanged notification.
    {
        const expectedEntryTemplates = [
            makeDropCollectionEntryTemplate(dbName, collName),
            makePlacementChangedEntryTemplate(collPlacementAfterDrop.timestamp, dbName, collName)
        ];

        const [commitEntry, placementChangedEntry] = verifyCommitOpEntriesOnShards(
            expectedEntryTemplates, [dataBearingShard])[dataBearingShard];
        // The commit entry must be visible to the end user.
        assert(!commitEntry.fromMigrate || commitEntry.fromMigrate === false);
        // The config.placementHistory doc references a cluster time within the range defined by the
        // creation of the two entries.
        assert(timestampCmp(commitEntry.ts, collPlacementAfterDrop.timestamp) < 0);
        assert(timestampCmp(collPlacementAfterDrop.timestamp, placementChangedEntry.ts) <= 0);
    }

    // 1.c) Non-data bearing shards have only generated a commit op entry, which is not visible to
    // the end user.
    {
        const nonDataBearingShards = shards.filter((shard) => shard !== dataBearingShard);
        const expectedEntryTemplates = [makeDropCollectionEntryTemplate(dbName, collName)];
        const retrievedEntriesByShard =
            verifyCommitOpEntriesOnShards(expectedEntryTemplates, nonDataBearingShards);
        for (const shardId in retrievedEntriesByShard) {
            const commitOpEntry = retrievedEntriesByShard[shardId][0];
            assert.eq(commitOpEntry.fromMigrate, true);
        }
    }

    // 2. Verify that no further placement document gets inserted if the drop is
    // repeated.
    assert.commandWorked(db.runCommand({drop: collName}));
    assert.eq(numHistoryEntriesAfterFirstDrop, configDB.placementHistory.count({nss: nss}));

    // 3. Verify that the creation and drop of an untracked collection leaves no trace in
    // config.placementHistory.
    const unshardedCollName = 'unshardedColl';
    assert.commandWorked(db.createCollection(unshardedCollName));
    assert.commandWorked(db.runCommand({drop: unshardedCollName}));
    assert.eq(0, configDB.placementHistory.count({nss: dbName + '.' + unshardedCollName}));
}

function testRenameCollection() {
    {
        jsTest.log(
            'Testing rename of sharded collection (located on the primary shard) with dropTarget=false');
        const dbName = 'renameShardedNoDropTargetTestDB';
        const db = st.s.getDB(dbName);
        const primaryShard = randomShard();
        const dataBearingShard = primaryShard;
        const sourceCollName = 'sourceColl';
        const sourceNss = dbName + '.' + sourceCollName;
        const targetCollName = 'targetColl';
        const targetNss = dbName + '.' + targetCollName;
        setupDbWithShardedCollection(dbName, sourceCollName, primaryShard, dataBearingShard);
        const sourcePlacementBeforeRename = getLatestPlacementInfoFor(sourceNss);

        assert.commandWorked(
            db[sourceCollName].renameCollection(targetCollName, false /*dropTarget*/));

        const sourcePlacementAfterRename = getLatestPlacementInfoFor(sourceNss);
        assert.eq(sourcePlacementBeforeRename.uuid, sourcePlacementAfterRename.uuid);
        assert.sameMembers([], sourcePlacementAfterRename.shards);

        const targetPlacementAfterRename = getLatestPlacementInfoFor(targetNss);
        assert.eq(sourcePlacementBeforeRename.uuid, targetPlacementAfterRename.uuid);
        assert.sameMembers(sourcePlacementBeforeRename.shards, targetPlacementAfterRename.shards);
        assert(timestampCmp(sourcePlacementBeforeRename.timestamp,
                            sourcePlacementAfterRename.timestamp) < 0);
        assert(timestampCmp(sourcePlacementAfterRename.timestamp,
                            targetPlacementAfterRename.timestamp) === 0);

        // 2.1 The data-bearing shard has to emit the user-visible commit op entry and the namespace
        // placement change for the source.
        // Being also the primary shard, it has to first emit the namespace placement change for the
        // target.
        const expectedEntryTemplatesOnDataBearingShard = [
            makePlacementChangedEntryTemplate(
                targetPlacementAfterRename.timestamp, dbName, targetCollName),
            makeRenameCollectionEntryTemplate(dbName, sourceCollName, targetCollName),
            makePlacementChangedEntryTemplate(
                sourcePlacementAfterRename.timestamp, dbName, sourceCollName),
        ];

        const [targetPlacementChangeEntry, renameCommitOpEntry, sourcePlacementChangeEntry] =
            verifyCommitOpEntriesOnShards(expectedEntryTemplatesOnDataBearingShard,
                                          [dataBearingShard])[dataBearingShard];
        assert(!renameCommitOpEntry.fromMigrate || renameCommitOpEntry.fromMigrate === false);

        // For renameCollection, the timestamps persisted in the config.placementHistory docs
        // precede the timestamps associated to each op entry.
        assert(timestampCmp(targetPlacementAfterRename.timestamp, targetPlacementChangeEntry.ts) <
               0);
        assert(timestampCmp(targetPlacementAfterRename.timestamp, renameCommitOpEntry.ts) < 0);
        assert(timestampCmp(sourcePlacementAfterRename.timestamp, sourcePlacementChangeEntry.ts) <=
               0);

        // 2.2 Other shards have to emit a non-visible commit op entry.
        const nonDataBearingShards = shards.filter((shard) => shard !== dataBearingShard);
        const expectedEntryTemplatesOnOtherShards =
            [makeRenameCollectionEntryTemplate(dbName, sourceCollName, targetCollName)];

        const retrievedEntriesByShard = verifyCommitOpEntriesOnShards(
            expectedEntryTemplatesOnOtherShards, nonDataBearingShards);
        for (const shardId in retrievedEntriesByShard) {
            const commitOpEntry = retrievedEntriesByShard[shardId][0];
            assert.eq(commitOpEntry.fromMigrate, true);
        }
    }

    {
        jsTest.log(
            'Testing rename of unsplittable collections outside of their primary shard with dropTarget=true');
        const sourceDbName = 'renameWithDropUnsplittableSourceDB';
        const targetDbName = 'renameWithDropUnsplittableTargetDB';
        const dbSource = st.s.getDB(sourceDbName);
        const dbTarget = st.s.getDB(targetDbName);
        const sourceCollName = 'sourceColl';
        const sourceNss = sourceDbName + '.' + sourceCollName;
        const targetCollName = 'targetColl';
        const targetNss = targetDbName + '.' + targetCollName;

        const commonPrimaryShard = shards[0];
        const sourceDataBearingShard = shards[1];
        const targetDataBearingShard = shards[2];
        assert.commandWorked(
            st.s.adminCommand({enableSharding: sourceDbName, primaryShard: commonPrimaryShard}));
        assert.commandWorked(
            st.s.adminCommand({enableSharding: targetDbName, primaryShard: commonPrimaryShard}));
        assert.commandWorked(dbSource.runCommand(
            {createUnsplittableCollection: sourceCollName, dataShard: sourceDataBearingShard}));
        assert.commandWorked(dbTarget.runCommand(
            {createUnsplittableCollection: targetCollName, dataShard: targetDataBearingShard}));
        const sourcePlacementBeforeRename = getValidatedPlacementInfoForCollection(
            sourceDbName, sourceCollName, [sourceDataBearingShard]);
        const targetPlacementBeforeRename = getValidatedPlacementInfoForCollection(
            targetDbName, targetCollName, [targetDataBearingShard]);

        assert.commandWorked(
            dbSource.adminCommand({renameCollection: sourceNss, to: targetNss, dropTarget: true}));

        const sourcePlacementAfterRename = getLatestPlacementInfoFor(sourceNss);
        const targetPlacementAfterRename = getLatestPlacementInfoFor(targetNss);
        assert.eq(sourcePlacementBeforeRename.uuid, sourcePlacementAfterRename.uuid);
        assert.sameMembers([], sourcePlacementAfterRename.shards);

        assert.sameMembers(sourcePlacementBeforeRename.shards, targetPlacementAfterRename.shards);
        // Cross-DB rename ops do not maintain the uuid of the source collection.
        assert.neq(sourcePlacementAfterRename.uuid, targetPlacementAfterRename.uuid);
        assert(timestampCmp(sourcePlacementAfterRename.timestamp,
                            targetPlacementAfterRename.timestamp) === 0);
        const commitTime = sourcePlacementAfterRename.timestamp;

        // When a cross-DB rename is performed, the request will involve the use of a temporary
        // collection under the target DB with a randomly generated name.
        // First retrieve its value from the oplog...
        const tempCollName = replicaSetByShardId[sourceDataBearingShard]
                                 .getPrimary()
                                 .getCollection('local.oplog.rs')
                                 .find({
                                     op: 'c',
                                     ns: `${targetDbName}.$cmd`,
                                     'o.create': {$regex: /^tmp.*renameCollection$/}
                                 })
                                 .sort({ts: -1})
                                 .limit(1)
                                 .toArray()[0]
                                 .o.create;
        const expectedEntryTemplatesOnSourceDataBearingShard = [
            makeCreateCollectionEntryTemplate(targetDbName, tempCollName),
            makeRenameCollectionEntryTemplate(targetDbName, tempCollName, targetCollName),
            // The source collection gets dropped...
            makeDropCollectionEntryTemplate(sourceDbName, sourceCollName),
            // ... and the placement change gets notified.
            makePlacementChangedEntryTemplate(commitTime, sourceDbName, sourceCollName)
        ];

        const [tempCollCreationEntry,
               renameCommitOpEntry,
               dropSourceCollCommitEntry,
               sourcePlacementChangeEntry] =
            verifyCommitOpEntriesOnShards(expectedEntryTemplatesOnSourceDataBearingShard,
                                          [sourceDataBearingShard])[sourceDataBearingShard];

        // All commit entries must be visible to the end user.
        assert(!tempCollCreationEntry.fromMigrate || tempCollCreationEntry.fromMigrate === false);
        assert(!renameCommitOpEntry.fromMigrate || renameCommitOpEntry.fromMigrate === false);
        assert(!dropSourceCollCommitEntry.fromMigrate ||
               dropSourceCollCommitEntry.fromMigrate === false);

        // The data bearing shard or the target collection has no knowledge of the source - due
        // to this, the commit will be matched by a non-visible notification of a
        // dropCollection.
        const expectedEntryTemplatesOnTargetDataBearingShard = [
            makePlacementChangedEntryTemplate(commitTime, targetDbName, targetCollName),
            makeDropCollectionEntryTemplate(targetDbName, targetCollName),
        ];

        const [targetPlacementChangeEntry, commitDropTargetOpEntry] =
            verifyCommitOpEntriesOnShards(expectedEntryTemplatesOnTargetDataBearingShard,
                                          [targetDataBearingShard])[targetDataBearingShard];
        assert.eq(commitDropTargetOpEntry.fromMigrate, true);

        // The placement change is first notified on the target collection, so that change stream
        // readers get redirected to the data bearing shard of the source to see the effect of the
        // rename.
        assert(timestampCmp(targetPlacementChangeEntry.ts, sourcePlacementChangeEntry.ts) < 0);
    }

    {
        jsTest.log(
            'Testing rename of an unsharded collection (with drop of a sharded one placed outside the primary shard)');
        const dbName = 'renameUnshardedCollTestDB';
        const db = st.s.getDB(dbName);
        const sourceCollName = 'sourceColl';
        const targetCollName = 'targetColl';
        const primaryShard = randomShard();
        const sourceDataBearingShard = primaryShard;
        const targetDataBearingShard = shards.find((shard) => shard !== primaryShard);

        setupDbWithShardedCollection(dbName, targetCollName, primaryShard, targetDataBearingShard);
        assert.commandWorked(db.createCollection(sourceCollName));
        const sourceCollUuidBeforeRename =
            db.getCollectionInfos({name: sourceCollName})[0].info.uuid;

        const targetPlacementBeforeRename =
            getLatestPlacementInfoFor(dbName + '.' + targetCollName);
        assert.sameMembers([targetDataBearingShard], targetPlacementBeforeRename.shards);

        assert.commandWorked(
            db[sourceCollName].renameCollection(targetCollName, true /*dropTarget*/));

        // config.placementHistory should contain no document on the source collection.
        assert.eq(null, getLatestPlacementInfoFor(dbName + '.' + sourceCollName));

        const targetPlacementAfterRename = getLatestPlacementInfoFor(dbName + '.' + targetCollName);
        assert.sameMembers([], targetPlacementAfterRename.shards);
        assert.eq(sourceCollUuidBeforeRename, targetPlacementAfterRename.uuid);

        // On the source shard,
        const expectedEntryTemplatesOnSourceDataBearingShard = [
            makeRenameCollectionEntryTemplate(
                dbName, sourceCollName, targetCollName, targetPlacementBeforeRename.uuid),
            makePlacementChangedEntryTemplate(
                targetPlacementAfterRename.timestamp, dbName, sourceCollName)
        ];

        const [renameCommitOpEntry, sourcePlacementChangeEntry] =
            verifyCommitOpEntriesOnShards(expectedEntryTemplatesOnSourceDataBearingShard,
                                          [sourceDataBearingShard])[sourceDataBearingShard];
        assert(!renameCommitOpEntry.fromMigrate || renameCommitOpEntry.fromMigrate === false);

        const expectedEntryTemplatesOnTargetDataBearingShard = [
            makePlacementChangedEntryTemplate(
                targetPlacementAfterRename.timestamp, dbName, targetCollName),
            makeDropCollectionEntryTemplate(dbName, targetCollName)
        ];

        const [targetPlacementChangeEntry, commitDropTargetOpEntry] =
            verifyCommitOpEntriesOnShards(expectedEntryTemplatesOnTargetDataBearingShard,
                                          [targetDataBearingShard])[targetDataBearingShard];
        assert.eq(commitDropTargetOpEntry.fromMigrate, true);
    }
}

function testDropDatabase() {
    const dbName = 'dropDatabaseTestDB';
    const db = st.s.getDB(dbName);

    // Test case 1: a database containing a sharded collection with a single chunk placed on the
    // primary shard.
    {
        const shardedCollName = 'shardedColl';
        const shardedCollNss = dbName + '.' + shardedCollName;
        const primaryShard = shards[0];
        const dataBearingShard = primaryShard;
        setupDbWithShardedCollection(dbName, shardedCollName, primaryShard, dataBearingShard);

        const collUUID = configDB.collections.findOne({_id: shardedCollNss}).uuid;

        assert.commandWorked(db.dropDatabase());

        const dbDropTimeInPlacementHistory =
            getValidatedPlacementInfoForDroppedDb(dbName).timestamp;
        const collDropTimeInPlacementHistory =
            getValidatedPlacementInfoForDroppedColl(shardedCollNss, collUUID).timestamp;

        // The primary shard (being also a data-bearing one) should have generated a pair of entries
        // for each dropped namespace.
        let expectedEntryTemplates = [
            makeDropCollectionEntryTemplate(dbName, shardedCollName),
            makePlacementChangedEntryTemplate(
                collDropTimeInPlacementHistory, dbName, shardedCollName),
            makeDropDatabaseEntryTemplate(dbName),
            makePlacementChangedEntryTemplate(dbDropTimeInPlacementHistory, dbName)
        ];

        const [commitCollDropEntry,
               collPlacementChangedEntry,
               commitDbOpEntry,
               dbPlacementChangedEntry] =
            verifyCommitOpEntriesOnShards(expectedEntryTemplates, [primaryShard])[primaryShard];

        // Each commit entry must be visible to the end user.
        assert(!commitCollDropEntry.fromMigrate || commitCollDropEntry.fromMigrate === false);
        assert(!commitDbOpEntry.fromMigrate || commitDbOpEntry.fromMigrate === false);

        // Verify the expected sequence of timestamps for each dropped namespace.
        assert(timestampCmp(commitCollDropEntry.ts, collDropTimeInPlacementHistory) < 0);
        assert(timestampCmp(collDropTimeInPlacementHistory, collPlacementChangedEntry.ts) <= 0);

        assert(timestampCmp(commitDbOpEntry.ts, dbDropTimeInPlacementHistory) < 0);
        assert(timestampCmp(dbDropTimeInPlacementHistory, dbPlacementChangedEntry.ts) <= 0);

        // Non-data-bearing shards have only generated one non-visible commit op entry for each
        // dropped namespace.
        const nonDataBearingShards = shards.filter((shard) => shard !== dataBearingShard);
        expectedEntryTemplates = [
            makeDropCollectionEntryTemplate(dbName, shardedCollName),
            makeDropDatabaseEntryTemplate(dbName)
        ];
        const retrievedEntriesByShard =
            verifyCommitOpEntriesOnShards(expectedEntryTemplates, nonDataBearingShards);
        for (const shardId in retrievedEntriesByShard) {
            const commitDropCollectionEntry = retrievedEntriesByShard[shardId][0];
            assert.eq(commitDropCollectionEntry.fromMigrate, true);
            const commitDropDatabaseEntry = retrievedEntriesByShard[shardId][1];
            assert.eq(commitDropDatabaseEntry.fromMigrate, true);
        }
    }

    // Test case 2 : a database containing an unsharded collection.
    const unshardedCollName = 'unshardedColl';
    const unshardedCollNss = dbName + '.' + unshardedCollName;
    const primaryShard = shards[0];
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShard}));

    assert.commandWorked(db.createCollection(unshardedCollName));

    assert.commandWorked(db.dropDatabase());

    // Verify that a placement change doc has been generated for the dropped database
    const dbDropTimeInPlacementHistory = getValidatedPlacementInfoForDroppedDb(dbName).timestamp;

    // ... but no placement change doc has been generated for the unsharded collection.
    assert.eq(null, getLatestPlacementInfoFor(unshardedCollNss));

    // The drop of the unsharded collection should have generated no placement change event.
    let expectedEntryTemplates = [
        makeDropCollectionEntryTemplate(dbName, unshardedCollName),
        makeDropDatabaseEntryTemplate(dbName),
        makePlacementChangedEntryTemplate(dbDropTimeInPlacementHistory, dbName)
    ];

    const [commitCollDropEntry, commitDbOpEntry, dbPlacementChangedEntry] =
        verifyCommitOpEntriesOnShards(expectedEntryTemplates, [primaryShard])[primaryShard];

    // Each commit entry must be visible to the end user.
    assert(!commitCollDropEntry.fromMigrate || commitCollDropEntry.fromMigrate === false);
    assert(!commitDbOpEntry.fromMigrate || commitDbOpEntry.fromMigrate === false);

    assert(timestampCmp(commitDbOpEntry.ts, dbDropTimeInPlacementHistory) < 0);
    assert(timestampCmp(dbDropTimeInPlacementHistory, dbPlacementChangedEntry.ts) <= 0);

    // Non-primary shards have only generated one non-visible commit op entry for the dropped
    // database.
    const nonDataBearingShards = shards.filter((shard) => shard !== primaryShard);
    expectedEntryTemplates = [makeDropDatabaseEntryTemplate(dbName)];
    const retrievedEntriesByShard =
        verifyCommitOpEntriesOnShards(expectedEntryTemplates, nonDataBearingShards);
    for (const shardId in retrievedEntriesByShard) {
        const commitDropDatabaseEntry = retrievedEntriesByShard[shardId][0];
        assert.eq(commitDropDatabaseEntry.fromMigrate, true);
    }

    // Test case 3: a database containing an unsplittable collection, located outside its primary
    // shard.
    {
        const unsplittableCollName = 'unsplittableColl';
        const unsplittableCollNss = dbName + '.' + unsplittableCollName;
        const primaryShard = shards[0];
        const dataBearingShard = shards[1];

        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShard}));
        assert.commandWorked(db.createCollection(unsplittableCollName));
        assert.commandWorked(
            st.s.adminCommand({moveCollection: unsplittableCollNss, toShard: dataBearingShard}));
        const collUUID = configDB.collections.findOne({_id: unsplittableCollNss}).uuid;

        assert.commandWorked(db.dropDatabase());

        const dbDropTimeInPlacementHistory =
            getValidatedPlacementInfoForDroppedDb(dbName).timestamp;
        const collDropTimeInPlacementHistory =
            getValidatedPlacementInfoForDroppedColl(unsplittableCollNss, collUUID).timestamp;

        // The primary shard is expected to generate an non-visible commit op entry for the
        // collection drop, plus the two entries for the database drop.
        let expectedEntryTemplates = [
            makeDropCollectionEntryTemplate(dbName, unsplittableCollName),
            makeDropDatabaseEntryTemplate(dbName),
            makePlacementChangedEntryTemplate(dbDropTimeInPlacementHistory, dbName)
        ];

        const [nonVisibleCommitCollDropEntry, commitDbOpEntry, dbPlacementChangedEntry] =
            verifyCommitOpEntriesOnShards(expectedEntryTemplates, [primaryShard])[primaryShard];

        assert(nonVisibleCommitCollDropEntry.fromMigrate === true);
        assert(!commitDbOpEntry.fromMigrate || commitDbOpEntry.fromMigrate === false);

        // Timestamps for the metadata around the db drop are the expected ones.
        assert(timestampCmp(commitDbOpEntry.ts, dbDropTimeInPlacementHistory) < 0);
        assert(timestampCmp(dbDropTimeInPlacementHistory, dbPlacementChangedEntry.ts) <= 0);

        // The data-bearing shard of the collection should have generated the two expected entries
        // for its drop, plus a non-visible commit op entry for the parent db.
        expectedEntryTemplates = [
            makeDropCollectionEntryTemplate(dbName, unsplittableCollName),
            makePlacementChangedEntryTemplate(
                collDropTimeInPlacementHistory, dbName, unsplittableCollName),
            makeDropDatabaseEntryTemplate(dbName)
        ];

        const [commitCollDropEntry, collPlacementChangedEntry, nonVisibleCommitDbOpEntry] =
            verifyCommitOpEntriesOnShards(expectedEntryTemplates,
                                          [dataBearingShard])[dataBearingShard];

        assert(!commitCollDropEntry.fromMigrate || commitCollDropEntry.fromMigrate === false);
        assert(nonVisibleCommitDbOpEntry.fromMigrate === true);

        // Timestamps for the metadata around the collection drop are the expected ones.
        assert(timestampCmp(commitCollDropEntry.ts, collDropTimeInPlacementHistory) < 0);
        assert(timestampCmp(collDropTimeInPlacementHistory, collPlacementChangedEntry.ts) <= 0);
    }
}

function testReshardCollection() {
    const dbName = 'reshardCollectionTestDB';
    const collName = 'shardedCollName';
    const nss = dbName + '.' + collName;
    let db = st.s.getDB(dbName);

    // Start with a collection with a single chunk on shard0.
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: shard0}));
    assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {oldShardKey: 1}}));
    const initialNumPlacementEntries = configDB.placementHistory.count({});
    const initialCollPlacementInfo =
        getValidatedPlacementInfoForCollection(dbName, collName, [shard0], true);

    // Create a set of zones that will force the new set of chunk to be distributed across all
    // the shards of the cluster.
    const zone1Name = 'zone1';
    assert.commandWorked(st.s.adminCommand({addShardToZone: shard1, zone: zone1Name}));
    const zone1Descriptor = {zone: zone1Name, min: {newShardKey: 0}, max: {newShardKey: 100}};
    const zone2Name = 'zone2';
    assert.commandWorked(st.s.adminCommand({addShardToZone: shard2, zone: zone2Name}));
    const zone2Descriptor = {zone: zone2Name, min: {newShardKey: 200}, max: {newShardKey: 300}};

    // Launch the reshard operation.
    assert.commandWorked(db.adminCommand({
        reshardCollection: nss,
        key: {newShardKey: 1},
        numInitialChunks: 1,
        zones: [zone1Descriptor, zone2Descriptor]
    }));

    // A single new placement document should have been added (the temp collection created by
    // resharding does not get tracked in config.placementHistory).
    assert.eq(1 + initialNumPlacementEntries, configDB.placementHistory.count({}));

    // Verify that the latest placement info matches the expectations.
    const finalCollPlacementInfo =
        getValidatedPlacementInfoForCollection(dbName, collName, [shard0, shard1, shard2], false);

    // The resharded collection maintains its nss, but changes its uuid.
    assert.neq(initialCollPlacementInfo.uuid, finalCollPlacementInfo.uuid);
}

jsTest.log('Verifying metadata generated by explicit DB creation');
testEnableSharding('explicitlyCreatedDB', shard0);

jsTest.log('Verifying metadata generated by shardCollection()');

testShardCollection();

jsTest.log('Verifying metadata generated by dropCollection()');
testDropCollection();

jsTest.log('Verifying metadata generated by a sequence of moveChunk() commands');
testMoveChunk('explicitlyCreatedDB', 'testMoveChunk');

jsTest.log('Verifying metadata generated by a sequence of moveRange() commands');
testMoveRange('explicitlyCreatedDB', 'testMoveRange');

jsTest.log(
    'Verifying metadata generated by movePrimary() over a new sharding-enabled DB with no data');
testMovePrimary('movePrimaryDB', st.shard0.shardName, st.shard1.shardName);

jsTest.log('Verifying metadata generated by renameCollection()');
testRenameCollection();

jsTest.log(
    'Verifying metadata generated by dropDatabase() over a new sharding-enabled DB with data');
testDropDatabase();

jsTest.log('Verifying metadata generated by reshardCollection()');
testReshardCollection();

// TODO (SERVER-100403): Remove mayTestAddShard once addShard registers dbs in the shard catalog.
const mayTestAddShard =
    !FeatureFlagUtil.isPresentAndEnabled(st.shard0, "ShardAuthoritativeDbMetadataDDL");

st.stop();

if (!mayTestAddShard || TestData.configShard) {
    jsTest.log('Skipping verification of addShard()');
    quit();
}

jsTest.log('Verifying metadata generated by addShard()');
st = new ShardingTest({
    shards: 0,
    chunkSize: 1,
    configOptions:
        {setParameter:
             {reshardingCriticalSectionTimeoutMillis: 24 * 60 * 60 * 1000, /* 1 day */}}
});

// Create a new replica set and populate it with some initial DBs
const newReplicaSet = new ReplSetTest({name: "addedShard", nodes: 1});
const newShardName = 'addedShard';
const preExistingCollName = 'preExistingColl';
newReplicaSet.startSet({shardsvr: ""});
newReplicaSet.initiate();
const dbsOnNewReplicaSet = ['addShardTestDB1', 'addShardTestDB2'];
for (const dbName of dbsOnNewReplicaSet) {
    const db = newReplicaSet.getPrimary().getDB(dbName);
    assert.commandWorked(db[preExistingCollName].save({value: 1}));
}

// Run addShard(); pre-existing collections in the replica set should be treated as unsharded
// collections, while their parent DBs should appear in config.placementHistory with consistent
// details.
assert.commandWorked(st.s.adminCommand({addShard: newReplicaSet.getURL(), name: newShardName}));

configDB = st.s.getDB('config');

for (const dbName of dbsOnNewReplicaSet) {
    assert.eq(null, getLatestPlacementInfoFor(dbName + '.' + preExistingCollName));

    const dbPlacementEntry = getValidatedPlacementInfoForDB(dbName);
    assert.sameMembers([newShardName], dbPlacementEntry.shards);
}

// Execute the test case teardown
for (const dbName of dbsOnNewReplicaSet) {
    assert.commandWorked(st.getDB(dbName).dropDatabase());
}

st.stop();
newReplicaSet.stopSet();
