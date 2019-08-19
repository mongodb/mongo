/**
 * Tests that renaming a collection is successful within a DB and across DBs.
 */
// TODO (SERVER-42394): The UUID consistency hook should not check shards' caches once shards have
// an "UNKNOWN" state for collection filtering metadata.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
'use strict';

load("jstests/sharding/libs/track_unsharded_collections_helpers.js");
load("jstests/sharding/libs/sharded_transactions_helpers.js");  // for waitForFailpoint

function runCollectionRenameWithConfigStepdownAtFailpointInShard(
    failpointName, fromColl, toColl, fromNs, toNs, node) {
    st.configRS.awaitNodesAgreeOnPrimary();
    const configPrimary = st.configRS.getPrimary();

    clearRawMongoProgramOutput();
    setFailpoint(failpointName, node);

    const renameCode = `assert.commandWorked(db.getSiblingDB("${
        dbName}").adminCommand({renameCollection: "${fromNs}", to: "${toNs}"}));`;
    const awaitResult = startParallelShell(renameCode, st.s.port);
    waitForFailpoint("Hit " + failpointName, 1);
    assert.commandWorked(configPrimary.adminCommand({replSetStepDown: 1, force: true}));
    waitForFailpoint("Hit " + failpointName, 2);
    unsetFailpoint(failpointName, node);

    awaitResult();
}

function runCollectionRenameWithConfigStepdownAtFailpointInPrimaryConfigsvr(
    failpointName, fromColl, toColl, fromNs, toNs) {
    st.configRS.awaitNodesAgreeOnPrimary();
    const configPrimary = st.configRS.getPrimary();

    clearRawMongoProgramOutput();
    setFailpoint(failpointName, configPrimary);

    const renameCode = `assert.commandWorked(db.getSiblingDB("${
        dbName}").adminCommand({renameCollection: "${fromNs}", to: "${toNs}"}));`;
    const awaitResult = startParallelShell(renameCode, st.s.port);
    waitForFailpoint("Hit " + failpointName, 1);
    assert.commandWorked(configPrimary.adminCommand({replSetStepDown: 1, force: true}));
    unsetFailpoint(failpointName, configPrimary);

    awaitResult();
}

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    other: {
        mongosOptions: {
            setParameter:
                {"failpoint.useRenameCollectionPathThroughConfigsvr": "{mode: 'alwaysOn'}"}
        },
        configOptions: {
            setParameter:
                {"failpoint.writeUnshardedCollectionsToShardingCatalog": "{mode: 'alwaysOn'}"}
        },
        shardOptions: {
            setParameter:
                {"failpoint.useRenameCollectionPathThroughConfigsvr": "{mode: 'alwaysOn'}"}
        }
    }
});
const dbName = "test";
const otherDbName = "other";

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({enableSharding: otherDbName}));
st.ensurePrimaryShard(otherDbName, st.shard0.shardName);

// Test renaming within a db when target doesn't exist.
(() => {
    let db = st.s.getDB(dbName);
    const [fromCollName, fromNs] = getNewNs(dbName);
    const [toCollName, toNs] = getNewNs(dbName);
    assert.commandWorked(db[fromCollName].insert({_id: 1}));
    jsTest.log(
        `Check renaming a collection within the same database; fromNS: ${fromNs}; toNS: ${toNs}`);

    checkInShardingCatalog({
        ns: fromNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkNotInShardingCatalog({ns: toCollName, mongosConn: st.s});
    checkInStorageCatalog(
        {dbName: dbName, collName: fromCollName, type: "collection", shardConn: st.shard0});

    assert.commandWorked(db[fromCollName].renameCollection(toCollName));
    assert.eq(db[toCollName].findOne(), {_id: 1});

    checkNotInShardingCatalog({ns: fromNs, mongosConn: st.s});
    checkInShardingCatalog({
        ns: toNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog(
        {dbName: dbName, collName: toCollName, type: "collection", shardConn: st.shard0});
    checkNotInStorageCatalog(
        {dbName: dbName, collName: fromCollName, type: "collection", shardConn: st.shard0});
})();

// Test renaming within a db when target exists.
(() => {
    let db = st.s.getDB(dbName);
    const [fromCollName, fromNs] = getNewNs(dbName);
    const [toCollName, toNs] = getNewNs(dbName);
    assert.commandWorked(db[fromCollName].insert({_id: 1}));
    assert.commandWorked(db[toCollName].insert({_id: 2}));
    jsTest.log(
        `Check renaming a collection within DB while target exists will fail; fromNS: \
${fromNs}; toNS: ${toNs}`);

    checkInShardingCatalog({
        ns: fromNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInShardingCatalog({
        ns: toNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog(
        {dbName: dbName, collName: fromCollName, type: "collection", shardConn: st.shard0});
    checkInStorageCatalog(
        {dbName: dbName, collName: toCollName, type: "collection", shardConn: st.shard0});

    assert.commandFailedWithCode(db.adminCommand({renameCollection: fromNs, to: toNs}),
                                 ErrorCodes.NamespaceExists);
    assert.eq(db[fromCollName].findOne(), {_id: 1});
    assert.eq(db[toCollName].findOne(), {_id: 2});

    checkInShardingCatalog({
        ns: fromNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInShardingCatalog({
        ns: toNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog(
        {dbName: dbName, collName: toCollName, type: "collection", shardConn: st.shard0});
    checkInStorageCatalog(
        {dbName: dbName, collName: fromCollName, type: "collection", shardConn: st.shard0});
})();

// Test renaming within a db when target exists with dropTarget passed.
(() => {
    let db = st.s.getDB(dbName);
    const [fromCollName, fromNs] = getNewNs(dbName);
    const [toCollName, toNs] = getNewNs(dbName);
    assert.commandWorked(db[fromCollName].insert({_id: 1}));
    assert.commandWorked(db[toCollName].insert({_id: 2}));
    jsTest.log(
        `Check renaming a collection within DB while target exists and target is dropped; \
fromNS: ${fromNs}; toNS: ${toNs}`);

    checkInShardingCatalog({
        ns: fromNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInShardingCatalog({
        ns: toNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog(
        {dbName: dbName, collName: fromCollName, type: "collection", shardConn: st.shard0});
    checkInStorageCatalog(
        {dbName: dbName, collName: toCollName, type: "collection", shardConn: st.shard0});

    assert.commandWorked(db.adminCommand({renameCollection: fromNs, to: toNs, dropTarget: true}));
    assert.eq(db[toCollName].findOne(), {_id: 1});

    checkNotInShardingCatalog({ns: fromNs, mongosConn: st.s});
    checkInShardingCatalog({
        ns: toNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog(
        {dbName: dbName, collName: toCollName, type: "collection", shardConn: st.shard0});
    checkNotInStorageCatalog(
        {dbName: dbName, collName: fromCollName, type: "collection", shardConn: st.shard0});
})();

// Test renaming across dbs when target doesn't exist.
(() => {
    let db = st.s.getDB(dbName);
    let otherDb = st.s.getDB(otherDbName);
    const [fromCollName, fromNs] = getNewNs(dbName);
    const [toCollName, toNs] = getNewNs(otherDbName);
    assert.commandWorked(db[fromCollName].insert({_id: 1}));
    assert.commandWorked(otherDb[fromCollName].insert({_id: 2}));
    jsTest.log(`Check renaming a collection across databases; fromNS: ${fromNs}; toNS: ${toNs}`);

    checkInShardingCatalog({
        ns: fromNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkNotInShardingCatalog({ns: toCollName, mongosConn: st.s});
    checkInStorageCatalog(
        {dbName: dbName, collName: fromCollName, type: "collection", shardConn: st.shard0});

    assert.commandWorked(db.adminCommand({renameCollection: fromNs, to: toNs}));
    assert.eq(otherDb[toCollName].findOne(), {_id: 1});

    checkNotInShardingCatalog({ns: fromNs, mongosConn: st.s});
    checkInShardingCatalog({
        ns: toNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog(
        {dbName: otherDbName, collName: toCollName, type: "collection", shardConn: st.shard0});
    checkNotInStorageCatalog(
        {dbName: dbName, collName: fromCollName, type: "collection", shardConn: st.shard0});
})();

// Test renaming across dbs when target exists.
(() => {
    let db = st.s.getDB(dbName);
    let otherDb = st.s.getDB(otherDbName);
    const [fromCollName, fromNs] = getNewNs(dbName);
    const [toCollName, toNs] = [fromCollName, otherDbName + "." + fromCollName];
    assert.commandWorked(db[fromCollName].insert({_id: 1}));
    assert.commandWorked(otherDb[toCollName].insert({_id: 2}));
    jsTest.log(
        `Check renaming a collection across databases while target exists will fail; fromNS: \
${fromNs}; toNS: ${toNs}`);

    checkInShardingCatalog({
        ns: fromNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInShardingCatalog({
        ns: toNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog(
        {dbName: dbName, collName: fromCollName, type: "collection", shardConn: st.shard0});
    checkInStorageCatalog(
        {dbName: otherDbName, collName: toCollName, type: "collection", shardConn: st.shard0});

    assert.commandFailedWithCode(db.adminCommand({renameCollection: fromNs, to: toNs}),
                                 ErrorCodes.NamespaceExists);
    assert.eq(db[fromCollName].findOne(), {_id: 1});
    assert.eq(otherDb[toCollName].findOne(), {_id: 2});

    checkInShardingCatalog({
        ns: fromNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInShardingCatalog({
        ns: toNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog(
        {dbName: dbName, collName: fromCollName, type: "collection", shardConn: st.shard0});
    checkInStorageCatalog(
        {dbName: otherDbName, collName: toCollName, type: "collection", shardConn: st.shard0});
})();

// Test renaming across dbs when target exists with dropTarget passed.
(() => {
    let db = st.s.getDB(dbName);
    let otherDb = st.s.getDB(otherDbName);
    const [fromCollName, fromNs] = getNewNs(dbName);
    const [toCollName, toNs] = getNewNs(otherDbName);
    assert.commandWorked(db[fromCollName].insert({_id: 1}));
    assert.commandWorked(otherDb[toCollName].insert({_id: 2}));
    jsTest.log(
        `Check renaming a collection across databases while target exists and target is dropped; \
fromNS: ${fromNs}; toNS: ${toNs}`);

    checkInShardingCatalog({
        ns: fromNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInShardingCatalog({
        ns: toNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog(
        {dbName: dbName, collName: fromCollName, type: "collection", shardConn: st.shard0});
    checkInStorageCatalog(
        {dbName: otherDbName, collName: toCollName, type: "collection", shardConn: st.shard0});

    assert.commandWorked(db.adminCommand({renameCollection: fromNs, to: toNs, dropTarget: true}));
    assert.eq(otherDb[toCollName].findOne(), {_id: 1});

    checkInShardingCatalog({
        ns: toNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });

    checkNotInShardingCatalog({ns: fromNs, mongosConn: st.s});
    checkInStorageCatalog(
        {dbName: otherDbName, collName: toCollName, type: "collection", shardConn: st.shard0});
    checkNotInStorageCatalog(
        {dbName: dbName, collName: fromCollName, type: "collection", shardConn: st.shard0});
})();

// Checking config stepdown behavior when duplicate commands need to be joined.
(() => {
    let db = st.s.getDB(dbName);
    const [fromCollName, fromNs] = getNewNs(dbName);
    const [toCollName, toNs] = getNewNs(dbName);
    jsTest.log(
        `Check config stepdown during renameCollection while shard is attempting the rename; \
fromNS: ${fromNs}; toNS: ${toNs}`);
    assert.commandWorked(db[fromCollName].insert({_id: 1}));

    checkInShardingCatalog({
        ns: fromNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkNotInShardingCatalog({ns: toCollName, mongosConn: st.s});

    runCollectionRenameWithConfigStepdownAtFailpointInShard(
        "hangRenameCollectionAfterGettingRename",
        fromCollName,
        toCollName,
        fromNs,
        toNs,
        st.rs0.getPrimary());

    checkInShardingCatalog({
        ns: toNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkNotInShardingCatalog({ns: fromNs, mongosConn: st.s});
    checkInStorageCatalog(
        {dbName: dbName, collName: toCollName, type: "collection", shardConn: st.shard0});
    checkNotInStorageCatalog(
        {dbName: dbName, collName: fromCollName, type: "collection", shardConn: st.shard0});
})();

// Checking config stepdown behavior when a rename has already been applied on a shard.
(() => {
    let db = st.s.getDB(dbName);
    const [fromCollName, fromNs] = getNewNs(dbName);
    const [toCollName, toNs] = getNewNs(dbName);
    jsTest.log(
        `Check config stepdown during renameCollection after sending renameCollection to the primary \
shard; fromNS: ${fromNs}; toNS: ${toNs}`);
    assert.commandWorked(db[fromCollName].insert({_id: 1}));

    checkInShardingCatalog({
        ns: fromNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkNotInShardingCatalog({ns: toCollName, mongosConn: st.s});

    runCollectionRenameWithConfigStepdownAtFailpointInPrimaryConfigsvr(
        "hangRenameCollectionAfterSendingRenameToPrimaryShard",
        fromCollName,
        toCollName,
        fromNs,
        toNs,
        st.configRS.getPrimary());

    checkInShardingCatalog({
        ns: toNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkNotInShardingCatalog({ns: fromNs, mongosConn: st.s});
    checkInStorageCatalog(
        {dbName: dbName, collName: toCollName, type: "collection", shardConn: st.shard0});
    checkNotInStorageCatalog(
        {dbName: dbName, collName: fromCollName, type: "collection", shardConn: st.shard0});
})();

// Test renaming across dbs on different primary shards.
(() => {
    let db = st.s.getDB(dbName);
    let otherDb = st.s.getDB(otherDbName);
    const [fromCollName, fromNs] = getNewNs(dbName);
    const [toCollName, toNs] = getNewNs(otherDbName);
    jsTest.log(
        `Check renaming a collection across databases while databases are on different primary shards \
will fail; fromNS: ${fromNs}; toNS: ${toNs}`);
    assert.commandWorked(db[fromCollName].insert({_id: 1}));
    assert.commandWorked(otherDb[fromCollName].insert({_id: 2}));

    // DB other's primary shard is now shard1 and DB test's primary is on shard0.
    assert.commandWorked(otherDb.adminCommand({movePrimary: otherDbName, to: st.shard1.shardName}));

    checkInShardingCatalog({
        ns: fromNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkNotInShardingCatalog({ns: toCollName, mongosConn: st.s});
    checkInStorageCatalog(
        {dbName: dbName, collName: fromCollName, type: "collection", shardConn: st.shard0});
    checkNotInStorageCatalog(
        {dbName: otherDbName, collName: toCollName, type: "collection", shardConn: st.shard0});
    checkNotInStorageCatalog(
        {dbName: otherDbName, collName: toCollName, type: "collection", shardConn: st.shard1});

    assert.commandFailedWithCode(db.adminCommand({renameCollection: fromNs, to: toNs}),
                                 ErrorCodes.IllegalOperation);
    assert.eq(db[fromCollName].findOne(), {_id: 1});

    checkInShardingCatalog({
        ns: fromNs,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkNotInShardingCatalog({ns: toCollName, mongosConn: st.s});
    checkInStorageCatalog(
        {dbName: dbName, collName: fromCollName, type: "collection", shardConn: st.shard0});
    checkNotInStorageCatalog(
        {dbName: otherDbName, collName: toCollName, type: "collection", shardConn: st.shard0});
    checkNotInStorageCatalog(
        {dbName: otherDbName, collName: toCollName, type: "collection", shardConn: st.shard1});
})();

st.stop();
})();
