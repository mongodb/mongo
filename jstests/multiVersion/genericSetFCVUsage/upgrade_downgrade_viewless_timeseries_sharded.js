/**
 * TODO (SERVER-116499): Remove this file once 9.0 becomes last LTS.
 *
 * Tests upgrading and downgrading timeseries collections between viewless and legacy formats.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_sharding,
 *   featureFlagCreateViewlessTimeseriesCollections,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

if (lastLTSFCV != "8.0") {
    jsTest.log.info("Skipping test because last LTS FCV is no longer 8.0");
    quit();
}

// Checks if system.buckets.<collName> exists, indicating legacy (viewful) format.
// Returns true if legacy format (buckets collection exists), false if viewless format.
function isLegacyTimeseriesFormat(adminDB, dbName, collName) {
    return (
        adminDB
            .getSiblingDB(dbName)
            .getCollection("system.buckets." + collName)
            .exists() !== null
    );
}

// Asserts all timeseries collections are in the expected format.
function assertTimeseriesFormat(adminDB, dbName, collectionsToCheck, expectLegacyFormat) {
    const formatName = expectLegacyFormat ? "legacy (viewful)" : "viewless";
    jsTest.log.info(`Verifying all collections are in ${formatName} format`);

    for (const {name, collName} of collectionsToCheck) {
        const actuallyLegacy = isLegacyTimeseriesFormat(adminDB, dbName, collName);
        assert.eq(
            expectLegacyFormat,
            actuallyLegacy,
            `${name} collection should be in ${formatName} format ` +
                `(system.buckets.${collName} ${actuallyLegacy ? "exists" : "does not exist"})`,
        );
    }
}

// Checks if the main namespace exists as a timeseries on a specific shard.
function mainNamespaceExistsOnShard(shard, dbName, collName) {
    const shardDB = shard.getDB(dbName);
    const info = shardDB.getCollectionInfos({name: collName, type: "timeseries"});
    return info.length > 0;
}

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

const dbName = jsTestName();
const db = st.s.getDB(dbName);
const adminDB = st.s.getDB("admin");
const configDB = st.s.getDB("config");

// Verify feature flag is enabled (collections will be created in viewless format)
assert(FeatureFlagUtil.isPresentAndEnabled(db, "CreateViewlessTimeseriesCollections"));
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

// Helper to perform CRUD operations and verify they work
function verifyCRUDOperations(collections) {
    for (const coll of collections) {
        const initialCount = coll.countDocuments({});

        // Insert
        const insertTime = ISODate();
        const insertResult = coll.insertOne({t: insertTime, m: 999, value: "inserted"});
        assert.commandWorked(insertResult);
        assert.eq(coll.countDocuments({}), initialCount + 1);

        // Find
        const findResult = coll.find({m: 999}).toArray();
        assert.eq(findResult.length, 1);
        assert.eq(findResult[0].value, "inserted");

        // Delete
        const deleteResult = coll.deleteMany({m: 999});
        assert.commandWorked(deleteResult);
        assert.eq(deleteResult.deletedCount, 1);
        assert.eq(coll.countDocuments({}), initialCount);
    }
}

// Helper to verify changelog entries exist for a given operation
function verifyChangelogEntries(collections) {
    for (const {name, collName} of collections) {
        const ns = dbName + "." + collName;
        const startLogCount = configDB.changelog.countDocuments({
            what: "upgradeDowngradeViewlessTimeseries.start",
            ns: ns,
        });
        assert.gte(startLogCount, 1, `Expected at least 1 start changelog entry for ${name}, found ${startLogCount}`);

        const endLogCount = configDB.changelog.countDocuments({
            what: "upgradeDowngradeViewlessTimeseries.end",
            ns: ns,
        });
        assert.gte(endLogCount, 1, `Expected at least 1 end changelog entry for ${name}, found ${endLogCount}`);
    }
}

// SETUP: Create timeseries collections (in viewless format)

jsTest.log.info("Creating sharded timeseries collection");
const shardedColl = db["shardedTs"];
assert.commandWorked(
    st.s.adminCommand({
        shardCollection: shardedColl.getFullName(),
        key: {m: 1},
        timeseries: {timeField: "t", metaField: "m"},
    }),
);
assert.commandWorked(
    st.s.adminCommand({
        moveRange: shardedColl.getFullName(),
        min: {meta: 200},
        max: {meta: MaxKey},
        toShard: st.shard1.shardName,
    }),
);
assert.commandWorked(
    shardedColl.insertMany([
        {t: ISODate(), m: 100, foo: 1},
        {t: ISODate(), m: 150, foo: 2},
        {t: ISODate(), m: 300, foo: 3},
        {t: ISODate(), m: 400, foo: 4},
    ]),
);

jsTest.log.info("Creating tracked unsharded timeseries collection");
const trackedColl = db["trackedTs"];
assert.commandWorked(
    db.runCommand({
        createUnsplittableCollection: "trackedTs",
        dataShard: st.shard1.shardName,
        timeseries: {timeField: "t", metaField: "m"},
    }),
);
assert.commandWorked(
    trackedColl.insertMany([
        {t: ISODate(), m: 500, foo: 5},
        {t: ISODate(), m: 600, foo: 6},
    ]),
);

jsTest.log.info("Creating untracked timeseries collection");
const untrackedColl = db["untrackedTs"];
assert.commandWorked(
    db.createCollection("untrackedTs", {
        timeseries: {timeField: "t", metaField: "m"},
    }),
);
assert.commandWorked(
    untrackedColl.insertMany([
        {t: ISODate(), m: 700, foo: 7},
        {t: ISODate(), m: 800, foo: 8},
    ]),
);

const collections = [
    {name: "sharded", collName: "shardedTs"},
    {name: "tracked", collName: "trackedTs"},
    {name: "untracked", collName: "untrackedTs"},
];
const userCollections = [shardedColl, trackedColl, untrackedColl];

jsTest.log.info("Verifying initial viewless format");
assertTimeseriesFormat(adminDB, dbName, collections, false /* expectLegacyFormat */);

// DOWNGRADE FCV: This will convert all timeseries to legacy format
assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

jsTest.log.info("Verifying all collections are in legacy format after downgrade");
assertTimeseriesFormat(adminDB, dbName, collections, true /* expectLegacyFormat */);

jsTest.log.info("Verifying changelog entries were created for downgrade");
verifyChangelogEntries(collections);

// Verify that the main namespace (timeseries view) only exists on the primary shard (shard0)
jsTest.log.info("Verifying main namespace (view) only exists on primary shard (shard0)");
const primaryShard = st.rs0.getPrimary();
const nonPrimaryShard = st.rs1.getPrimary();

assert(mainNamespaceExistsOnShard(primaryShard, dbName, "shardedTs"));
assert(!mainNamespaceExistsOnShard(nonPrimaryShard, dbName, "shardedTs"));
assert(mainNamespaceExistsOnShard(primaryShard, dbName, "trackedTs"));
assert(!mainNamespaceExistsOnShard(nonPrimaryShard, dbName, "trackedTs"));

jsTest.log.info("Verifying CRUD operations work after downgrade");
verifyCRUDOperations(userCollections);

jsTest.log.info("Testing idempotency: downgrading already legacy collection");
assert.commandWorked(
    db.runCommand({
        upgradeDowngradeViewlessTimeseries: shardedColl.getName(),
        mode: "downgradeToLegacy",
    }),
);
assertTimeseriesFormat(adminDB, dbName, [{name: "sharded", collName: "shardedTs"}], true /* expectLegacyFormat */);

// UPGRADE FCV: This will convert all timeseries back to viewless format
assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

jsTest.log.info("Verifying all collections are in viewless format after upgrade");
assertTimeseriesFormat(adminDB, dbName, collections, false /* expectLegacyFormat */);

jsTest.log.info("Verifying changelog entries were created for upgrade");
verifyChangelogEntries(collections);

jsTest.log.info("Verifying CRUD operations work after upgrade");
verifyCRUDOperations(userCollections);

jsTest.log.info("Testing idempotency: upgrading already viewless collection");
assert.commandWorked(
    db.runCommand({
        upgradeDowngradeViewlessTimeseries: shardedColl.getName(),
        mode: "upgradeToViewless",
    }),
);
assertTimeseriesFormat(adminDB, dbName, [{name: "sharded", collName: "shardedTs"}], false /* expectLegacyFormat */);

st.stop();
