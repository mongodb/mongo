/*
 * Tests to validate the correct behaviour of checkMetadataConsistency command.
 *
 * @tags: [
 *    requires_fcv_70,
 * ]
 */

// Configure initial sharding cluster
const st = new ShardingTest({});
const mongos = st.s;

// Use retryWrites when writing to the configsvr because mongos does not automatically retry those.
const mongosSession = mongos.startSession({retryWrites: true});
const configDB = mongosSession.getDatabase("config");

const dbName = "testCheckMetadataConsistencyDB";
var dbCounter = 0;

function getNewDb() {
    return mongos.getDB(dbName + dbCounter++);
}

function assertNoInconsistencies() {
    const checkOptions = {'checkIndexes': 1};

    let res = mongos.getDB("admin").checkMetadataConsistency(checkOptions).toArray();
    assert.eq(0,
              res.length,
              "Found unexpected metadata inconsistencies at cluster level: " + tojson(res));

    mongos.getDBNames().forEach(dbName => {
        if (dbName == 'admin') {
            return;
        }

        let db = mongos.getDB(dbName);
        res = db.checkMetadataConsistency(checkOptions).toArray();
        assert.eq(0,
                  res.length,
                  "Found unexpected metadata inconsistencies at database level: " + tojson(res));

        db.getCollectionNames().forEach(collName => {
            let coll = db.getCollection(collName);
            res = coll.checkMetadataConsistency(checkOptions).toArray();
            assert.eq(
                0,
                res.length,
                "Found unexpected metadata inconsistencies at collection level: " + tojson(res));
        });
    });
}

function assertCollectionOptionsMismatch(inconsistencies, expectedOptions) {
    assert(inconsistencies.some(object => {
        return (object.type === "CollectionOptionsMismatch" &&
                expectedOptions.every(expectedO => object.details.options.some(
                                          o => tojson(o.options) === tojson(expectedO))));
    }),
           "Expected CollectionOptionsMismatch options: " + tojson(expectedOptions) + ", but got " +
               tojson(inconsistencies));
}

// TODO SERVER-77915 We can get rid of isTrackedByConfigServer method once all unsharded collections
// are being tracked by the config server
function isTrackedByConfigServer(nss) {
    return configDB.collections.countDocuments({_id: nss}, {limit: 1}) !== 0;
}

function isFcvGraterOrEqualTo(fcvRequired) {
    // Requires all primary shard nodes to be running the fcvRequired version.
    let isFcvGreater = true;
    st.forEachConnection(function(conn) {
        const fcvDoc = conn.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
        if (MongoRunner.compareBinVersions(fcvDoc.featureCompatibilityVersion.version,
                                           fcvRequired) < 0) {
            isFcvGreater = false;
        }
    });
    return isFcvGreater;
}

(function testCursor() {
    const db = getNewDb();

    assert.commandWorked(
        mongos.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

    assert.commandWorked(st.shard1.getDB(db.getName()).coll1.insert({_id: 'foo'}));
    assert.commandWorked(st.shard1.getDB(db.getName()).coll2.insert({_id: 'foo'}));
    assert.commandWorked(st.shard1.getDB(db.getName()).coll3.insert({_id: 'foo'}));
    assert.commandWorked(st.shard1.getDB(db.getName()).coll4.insert({_id: 'foo'}));

    assert.commandWorked(
        st.s.adminCommand({shardCollection: db.coll1.getFullName(), key: {_id: 1}}));
    assert.commandWorked(
        st.s.adminCommand({shardCollection: db.coll2.getFullName(), key: {_id: 1}}));
    assert.commandWorked(
        st.s.adminCommand({shardCollection: db.coll3.getFullName(), key: {_id: 1}}));
    assert.commandWorked(
        st.s.adminCommand({shardCollection: db.coll4.getFullName(), key: {_id: 1}}));

    // Check correct behaviour of cursor with DBCommandCursor
    let res = db.runCommand({checkMetadataConsistency: 1, cursor: {batchSize: 1}});
    assert.commandWorked(res);

    assert.eq(1, res.cursor.firstBatch.length);
    const cursor = new DBCommandCursor(db, res);
    for (let i = 0; i < 4; i++) {
        assert(cursor.hasNext());
        const inconsistency = cursor.next();
        assert.eq(inconsistency.type, "CollectionUUIDMismatch");
    }
    assert(!cursor.hasNext());

    // Check correct behaviour of cursor with GetMore
    res = db.runCommand({checkMetadataConsistency: 1, cursor: {batchSize: 3}});
    assert.commandWorked(res);
    assert.eq(3, res.cursor.firstBatch.length);

    const getMoreCollName = res.cursor.ns.substr(res.cursor.ns.indexOf(".") + 1);
    res =
        assert.commandWorked(db.runCommand({getMore: res.cursor.id, collection: getMoreCollName}));
    assert.eq(1, res.cursor.nextBatch.length);

    // Clean up the database to pass the hooks that detect inconsistencies
    db.dropDatabase();
    assertNoInconsistencies();
})();

(function testCollectionUUIDMismatchInconsistency() {
    const db = getNewDb();

    assert.commandWorked(
        mongos.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

    assert.commandWorked(st.shard1.getDB(db.getName()).coll.insert({_id: 'foo'}));

    assert.commandWorked(
        st.s.adminCommand({shardCollection: db.coll.getFullName(), key: {_id: 1}}));

    // Database level mode command
    let inconsistencies = db.checkMetadataConsistency().toArray();
    assert.eq(1, inconsistencies.length, tojson(inconsistencies));
    assert.eq("CollectionUUIDMismatch", inconsistencies[0].type, tojson(inconsistencies[0]));

    // Collection level mode command
    const collInconsistencies = db.coll.checkMetadataConsistency().toArray();
    assert.eq(1, collInconsistencies.length);
    assert.eq(
        "CollectionUUIDMismatch", collInconsistencies[0].type, tojson(collInconsistencies[0]));

    // Clean up the database to pass the hooks that detect inconsistencies
    db.dropDatabase();
    assertNoInconsistencies();
})();

(function testMisplacedCollection() {
    const db = getNewDb();

    assert.commandWorked(
        mongos.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

    assert.commandWorked(st.shard1.getDB(db.getName()).coll.insert({_id: 'foo'}));

    // Database level mode command
    let inconsistencies = db.checkMetadataConsistency().toArray();
    assert.eq(1, inconsistencies.length, tojson(inconsistencies));
    assert.eq("MisplacedCollection", inconsistencies[0].type, tojson(inconsistencies[0]));

    // Collection level mode command
    const collInconsistencies = db.coll.checkMetadataConsistency().toArray();
    assert.eq(1, collInconsistencies.length, tojson(inconsistencies));
    assert.eq("MisplacedCollection", collInconsistencies[0].type, tojson(inconsistencies[0]));

    // Clean up the database to pass the hooks that detect inconsistencies
    db.dropDatabase();
    assertNoInconsistencies();
})();

(function testMissingShardKeyInconsistency() {
    const db = getNewDb();
    const kSourceCollName = "coll";

    st.shardColl(
        kSourceCollName, {skey: 1}, {skey: 0}, {skey: 1}, db.getName(), true /* waitForDelete */);

    // Connect directly to shards to bypass the mongos checks for dropping shard key indexes
    assert.commandWorked(st.shard0.getDB(db.getName()).coll.dropIndex({skey: 1}));
    assert.commandWorked(st.shard1.getDB(db.getName()).coll.dropIndex({skey: 1}));

    assert.commandWorked(st.s.getDB(db.getName()).coll.insert({skey: -10}));
    assert.commandWorked(st.s.getDB(db.getName()).coll.insert({skey: 10}));

    // Database level mode command
    let inconsistencies = db.checkMetadataConsistency().toArray();
    assert.eq(2, inconsistencies.length, tojson(inconsistencies));
    assert.eq("MissingShardKeyIndex", inconsistencies[0].type, tojson(inconsistencies[0]));
    assert.eq("MissingShardKeyIndex", inconsistencies[1].type, tojson(inconsistencies[1]));

    // Clean up the database to pass the hooks that detect inconsistencies
    db.dropDatabase();
    assertNoInconsistencies();
})();

(function testMissingIndex() {
    const db = getNewDb();
    const coll = db.coll;
    const shard0Coll = st.shard0.getDB(db.getName()).coll;
    const shard1Coll = st.shard1.getDB(db.getName()).coll;
    st.shardColl(coll, {skey: 1});

    const checkOptions = {'checkIndexes': 1};

    // Check missing index on one shard
    assert.commandWorked(coll.createIndex({key1: 1}, 'index1'));
    assert.commandWorked(shard1Coll.dropIndex('index1'));

    let inconsistencies = db.checkMetadataConsistency(checkOptions).toArray();
    assert.eq(1, inconsistencies.length, tojson(inconsistencies));
    assert.eq("InconsistentIndex", inconsistencies[0].type, tojson(inconsistencies));
    let collInconsistencies = coll.checkMetadataConsistency(checkOptions).toArray();
    assert.eq(sortDoc(inconsistencies), sortDoc(collInconsistencies));

    // Fix inconsistencies and assert none are left
    assert.commandWorked(shard0Coll.dropIndex('index1'));
    assertNoInconsistencies();

    // Check inconsistent index property across shards
    assert.commandWorked(shard0Coll.createIndex(
        {key1: 1}, {name: 'index1', sparse: true, expireAfterSeconds: 3600}));
    assert.commandWorked(shard1Coll.createIndex({key1: 1}, {name: 'index1', sparse: true}));

    inconsistencies = db.checkMetadataConsistency(checkOptions).toArray();
    assert.eq(1, inconsistencies.length, tojson(inconsistencies));
    assert.eq("InconsistentIndex", inconsistencies[0].type, tojson(inconsistencies));
    collInconsistencies = coll.checkMetadataConsistency(checkOptions).toArray();
    assert.eq(sortDoc(inconsistencies), sortDoc(collInconsistencies));

    // Fix inconsistencies and assert none are left
    assert.commandWorked(shard0Coll.dropIndex('index1'));
    assert.commandWorked(shard1Coll.dropIndex('index1'));
    assertNoInconsistencies();

    db.dropDatabase();
})();

(function testHiddenShardedCollections() {
    const kSourceCollName = "coll";
    const db1 = getNewDb();
    const coll1 = db1[kSourceCollName];
    const db2 = getNewDb();
    const coll2 = db2[kSourceCollName];

    // Create two sharded collections in two different databases
    st.shardColl(coll1, {skey: 1});
    st.shardColl(coll2, {skey: 1});

    // Save db1 and db2 configuration to restore it later
    const configDatabasesColl = mongos.getDB('config').databases;
    const db1ConfigEntry = configDatabasesColl.findOne({_id: db1.getName()});
    const db2ConfigEntry = configDatabasesColl.findOne({_id: db2.getName()});

    // Check that there are no inconsistencies so far
    assertNoInconsistencies();

    // Remove db1 so that coll1 became hidden
    assert.commandWorked(configDatabasesColl.deleteOne({_id: db1.getName()}));

    let inconsistencies = mongos.getDB("admin").checkMetadataConsistency().toArray();
    assert.eq(1, inconsistencies.length, tojson(inconsistencies));
    assert.eq("HiddenShardedCollection", inconsistencies[0].type, tojson(inconsistencies[0]));
    assert.eq(
        coll1.getFullName(), inconsistencies[0].details.namespace, tojson(inconsistencies[0]));

    // Remove db2 so that coll2 also became hidden
    assert.commandWorked(configDatabasesColl.deleteOne({_id: db2.getName()}));

    inconsistencies = mongos.getDB("admin").checkMetadataConsistency().toArray();
    assert.eq(2, inconsistencies.length, tojson(inconsistencies));
    assert.eq("HiddenShardedCollection", inconsistencies[0].type, tojson(inconsistencies[0]));
    assert.eq(
        coll1.getFullName(), inconsistencies[0].details.namespace, tojson(inconsistencies[0]));
    assert.eq("HiddenShardedCollection", inconsistencies[1].type, tojson(inconsistencies[1]));
    assert.eq(
        coll2.getFullName(), inconsistencies[1].details.namespace, tojson(inconsistencies[1]));

    // Restore db1 and db2 configuration to ensure the correct behavior of dropDatabase operations
    assert.commandWorked(configDatabasesColl.insertMany([db1ConfigEntry, db2ConfigEntry]));

    // Clean up the database to pass the hooks that detect inconsistencies
    db1.dropDatabase();
    db2.dropDatabase();
    assertNoInconsistencies();
})();

(function testRoutingTableInconsistency() {
    const db = getNewDb();
    const kSourceCollName = "coll";
    const ns = db[kSourceCollName].getFullName();

    st.shardColl(db[kSourceCollName], {skey: 1});

    // Insert a RoutingTableRangeOverlap inconsistency
    const collUuid = configDB.collections.findOne({_id: ns}).uuid;
    assert.commandWorked(configDB.chunks.updateOne({uuid: collUuid}, {$set: {max: {skey: 10}}}));

    // Insert a ZonesRangeOverlap inconsistency
    let entry = {
        _id: {ns: ns, min: {"skey": -100}},
        ns: ns,
        min: {"skey": -100},
        max: {"skey": 100},
        tag: "a",
    };
    assert.commandWorked(configDB.tags.insert(entry));
    entry = {
        _id: {ns: ns, min: {"skey": 50}},
        ns: ns,
        min: {"skey": 50},
        max: {"skey": 150},
        tag: "a",
    };
    assert.commandWorked(configDB.tags.insert(entry));

    // Database level mode command
    let inconsistencies = db.checkMetadataConsistency().toArray();
    assert.eq(2, inconsistencies.length, tojson(inconsistencies));
    assert(inconsistencies.some(object => object.type === "RoutingTableRangeOverlap"),
           tojson(inconsistencies));
    assert(inconsistencies.some(object => object.type === "ZonesRangeOverlap"),
           tojson(inconsistencies));

    // Clean up the database to pass the hooks that detect inconsistencies
    db.dropDatabase();
    inconsistencies = mongos.getDB("admin").checkMetadataConsistency().toArray();
    assert.eq(0, inconsistencies.length, tojson(inconsistencies));
})();

(function testClusterLevelMode() {
    const db_MisplacedCollection1 = getNewDb();
    const db_MisplacedCollection2 = getNewDb();
    const db_CollectionUUIDMismatch = getNewDb();

    // Insert MisplacedCollection inconsistency in db_MisplacedCollection1
    assert.commandWorked(mongos.adminCommand(
        {enableSharding: db_MisplacedCollection1.getName(), primaryShard: st.shard0.shardName}));
    assert.commandWorked(
        st.shard1.getDB(db_MisplacedCollection1.getName()).coll.insert({_id: 'foo'}));

    // Insert MisplacedCollection inconsistency in db_MisplacedCollection2
    assert.commandWorked(mongos.adminCommand(
        {enableSharding: db_MisplacedCollection2.getName(), primaryShard: st.shard1.shardName}));
    assert.commandWorked(
        st.shard0.getDB(db_MisplacedCollection2.getName()).coll.insert({_id: 'foo'}));

    // Insert CollectionUUIDMismatch inconsistency in db_CollectionUUIDMismatch
    assert.commandWorked(mongos.adminCommand(
        {enableSharding: db_CollectionUUIDMismatch.getName(), primaryShard: st.shard1.shardName}));

    assert.commandWorked(
        st.shard0.getDB(db_CollectionUUIDMismatch.getName()).coll.insert({_id: 'foo'}));

    assert.commandWorked(st.s.adminCommand(
        {shardCollection: db_CollectionUUIDMismatch.coll.getFullName(), key: {_id: 1}}));

    // Cluster level mode command
    let inconsistencies = mongos.getDB("admin").checkMetadataConsistency().toArray();

    // Check that there are 3 inconsistencies: 2 MisplacedCollection and 1 CollectionUUIDMismatch
    assert.eq(3, inconsistencies.length, tojson(inconsistencies));
    const count = inconsistencies.reduce((acc, object) => {
        return object.type === "MisplacedCollection" ? acc + 1 : acc;
    }, 0);
    assert.eq(2, count, tojson(inconsistencies));
    assert(inconsistencies.some(object => object.type === "CollectionUUIDMismatch"),
           tojson(inconsistencies));

    // Clean up the databases to pass the hooks that detect inconsistencies
    db_MisplacedCollection1.dropDatabase();
    db_MisplacedCollection2.dropDatabase();
    db_CollectionUUIDMismatch.dropDatabase();
    assertNoInconsistencies();
})();

(function testUnsplittableCollectionHas2Chunks() {
    const db = getNewDb();
    const kSourceCollName = "unsplittable_collection";
    const kNss = db.getName() + "." + kSourceCollName;
    // create a splittable collection with 2 chunks
    assert.commandWorked(db.adminCommand({shardCollection: kNss, key: {_id: 1}}));
    mongos.getCollection(kNss).insert({_id: 0});
    mongos.getCollection(kNss).insert({_id: 2});
    assert.commandWorked(mongos.adminCommand({split: kNss, middle: {_id: 1}}));

    let no_inconsistency = db.checkMetadataConsistency().toArray();
    assert.eq(no_inconsistency.length, 0);

    // make the collection unsplittable
    assert.commandWorked(configDB.collections.update({_id: kNss}, {$set: {unsplittable: true}}));

    let inconsistencies_chunks = db.checkMetadataConsistency().toArray();
    assert.eq(inconsistencies_chunks.length, 1);
    assert.eq("TrackedUnshardedCollectionHasMultipleChunks",
              inconsistencies_chunks[0].type,
              tojson(inconsistencies_chunks[0]));

    // Clean up the database to pass the hooks that detect inconsistencies
    db.dropDatabase();
    assertNoInconsistencies();
})();

(function testUnsplittableHasInvalidKey() {
    const db = getNewDb();
    const kSourceCollName = "unsplittable_collection";
    const kNss = db.getName() + "." + kSourceCollName;
    const primaryShard = st.shard1;

    // set a primary shard
    assert.commandWorked(
        mongos.adminCommand({enableSharding: db.getName(), primaryShard: primaryShard.shardName}));

    // create a splittable collection with a key != {_id:1}
    assert.commandWorked(db.adminCommand({shardCollection: kNss, key: {x: 1}}));

    let no_inconsistency = db.checkMetadataConsistency().toArray();
    assert.eq(no_inconsistency.length, 0);

    // make the collection unsplittable and catch the inconsistency
    assert.commandWorked(configDB.collections.update({_id: kNss}, {$set: {unsplittable: true}}));

    let inconsistencies_key = db.checkMetadataConsistency().toArray();
    assert.eq(1, inconsistencies_key.length);
    assert.eq("TrackedUnshardedCollectionHasInvalidKey",
              inconsistencies_key[0].type,
              tojson(inconsistencies_key[0]));

    // drop the collection on the primary and still catch the inconsistency
    primaryShard.getDB(db.getName()).runCommand({drop: kSourceCollName});
    let inconsistencies_key2 = db.checkMetadataConsistency().toArray();
    assert.eq(1, inconsistencies_key2.length);
    assert.eq("TrackedUnshardedCollectionHasInvalidKey",
              inconsistencies_key2[0].type,
              tojson(inconsistencies_key2[0]));

    // Clean up the database to pass the hooks that detect inconsistencies
    db.dropDatabase();
    assertNoInconsistencies();
})();

(function testTimeseriesOptionsMismatch() {
    // TODO SERVER-79304 Remove FCV check when 8.0 becomes last LTS.
    if (!isFcvGraterOrEqualTo('8.0')) {
        jsTestLog("Skipping timeseriesOptionsMismatch test because required FCV is less than 8.0.");
        return;
    }

    const db = getNewDb();
    const kSourceCollName = "tracked_collection";
    const kNss = db.getName() + "." + kSourceCollName;
    const kBucketNss = db.getName() + ".system.buckets." + kSourceCollName;

    // Create a timeseries sharded collection.
    assert.commandWorked(db.adminCommand({
        shardCollection: kNss,
        key: {time: 1},
        timeseries: {timeField: "time", metaField: "meta", granularity: "minutes"}
    }));
    assertNoInconsistencies();
    const localTimeseries = db.getCollectionInfos({name: kSourceCollName})[0].options.timeseries;
    assert.eq("minutes", localTimeseries.granularity);

    // Update the granularity on the sharding catalog only and catch the inconsistency.
    assert.commandWorked(configDB.collections.update(
        {_id: kBucketNss}, {$set: {'timeseriesFields.granularity': "seconds"}}));
    let configTimeseries = localTimeseries;
    configTimeseries.granularity = "seconds";

    const inconsistencies = db.checkMetadataConsistency().toArray();
    assert.eq(1, inconsistencies.length);
    assertCollectionOptionsMismatch(
        inconsistencies,
        [{timeseriesFields: localTimeseries}, {timeseriesFields: configTimeseries}]);

    // Clean up the database to pass the hooks that detect inconsistencies.
    db.dropDatabase();
    assertNoInconsistencies();
})();

(function testDefaultCollationMismatch1() {
    // TODO SERVER-79304 Remove FCV check when 8.0 becomes last LTS.
    if (!isFcvGraterOrEqualTo('8.0')) {
        jsTestLog(
            "Skipping testDefaultCollationMismatch2 test because required FCV is less than 8.0.");
        return;
    }

    const db = getNewDb();
    const kSourceCollName = "tracked_collection";
    const kNss = db.getName() + "." + kSourceCollName;

    // Create a collection with a specific default collation.
    assert.commandWorked(db.runCommand({create: kSourceCollName, collation: {locale: "ca"}}));
    const localCollation = db.getCollectionInfos({name: kSourceCollName})[0].options.collation;
    assertNoInconsistencies();

    if (!isTrackedByConfigServer(kNss)) {
        // Shard the collection to make it tracked.
        // Note: we need to specify a simple collation on shardCollection command to make clear that
        // chunks will be sorted using a simple collation, however, the default collation for the
        // collection is preserved to {'locale':'ca'}.
        assert.commandWorked(
            db.adminCommand({shardCollection: kNss, key: {x: 1}, collation: {locale: "simple"}}));

        const no_inconsistency = db.checkMetadataConsistency().toArray();
        assert.eq(0, no_inconsistency.length);
    }

    // Update the default collation on the sharding catalog only and catch the inconsistency.
    assert.commandWorked(
        configDB.collections.update({_id: kNss}, {$unset: {defaultCollation: ""}}));

    const inconsistencies = db.checkMetadataConsistency().toArray();
    assert.eq(1, inconsistencies.length);
    assertCollectionOptionsMismatch(inconsistencies,
                                    [{defaultCollation: localCollation}, {defaultCollation: {}}]);

    // Clean up the database to pass the hooks that detect inconsistencies.
    db.dropDatabase();
    assertNoInconsistencies();
})();

(function testCappedCollectionCantBeSharded() {
    // TODO SERVER-79304 Remove FCV check when 8.0 becomes last LTS.
    if (!isFcvGraterOrEqualTo('8.0')) {
        jsTestLog(
            "Skipping testCappedCollectionCantBeSharded test because required FCV is less than 8.0.");
        return;
    }

    const db = getNewDb();
    const kSourceCollName = "capped_collection";
    const kNss = db.getName() + "." + kSourceCollName;

    // Create a capped collection.
    assert.commandWorked(db.runCommand({create: kSourceCollName, capped: true, size: 1000}));
    assertNoInconsistencies();

    if (isTrackedByConfigServer(kNss)) {
        configDB.collections.update({_id: kNss}, {$set: {unsplittable: false}});

    } else {
        // Register another collection as sharded to be able to get a config.collections document as
        // a reference.
        const kNssSharded = db.getName() + ".sharded_collection";
        assert.commandWorked(db.adminCommand({shardCollection: kNssSharded, key: {x: 1}}));

        const uuid = db.getCollectionInfos({name: kSourceCollName})[0].info.uuid;

        // Insert a new collection into config.collections with the nss and uuid from the unsharded
        // capped collection previously created.
        let collEntry = configDB.collections.findOne({_id: kNssSharded});
        collEntry._id = kNss;
        collEntry.uuid = uuid;
        configDB.collections.insert(collEntry);
    }

    // Catch the inconsistency.
    const inconsistencies = db.checkMetadataConsistency().toArray();
    assert.neq(0, inconsistencies.length);
    assertCollectionOptionsMismatch(inconsistencies,
                                    [{capped: true}, {capped: false, unsplittable: false}]);

    // Clean up the database to pass the hooks that detect inconsistencies.
    db.dropDatabase();
    assertNoInconsistencies();
})();

st.stop();
