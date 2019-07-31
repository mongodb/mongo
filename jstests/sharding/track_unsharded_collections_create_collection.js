/**
 * Tests that creating unsharded collections correctly updates the sharding catalog.
 */

// TODO (SERVER-42394): The UUID consistency hook should not check shards' caches once shards have
// an "UNKNOWN" state for collection filtering metadata.
TestData.skipCheckingCatalogCacheConsistencyWithShardingCatalog = true;

(function() {
'use strict';

load("jstests/sharding/libs/track_unsharded_collections_helpers.js");
load("jstests/sharding/libs/sharded_transactions_helpers.js");  // for waitForFailpoint

function runExplicitCollectionCreationWithConfigStepdownAtFailpoint(failpointName, collName) {
    st.configRS.awaitNodesAgreeOnPrimary();
    const configPrimary = st.configRS.getPrimary();

    clearRawMongoProgramOutput();
    setFailpoint(failpointName, configPrimary);

    const explicitCreateCode =
        `assert.commandWorked(db.getSiblingDB("${dbName}").runCommand({create: "${collName}"}));`;
    const awaitResult = startParallelShell(explicitCreateCode, st.s.port);
    waitForFailpoint("Hit " + failpointName, 1);

    assert.commandWorked(configPrimary.adminCommand({replSetStepDown: 1, force: true}));
    unsetFailpoint(failpointName, configPrimary);

    awaitResult();
}

function runImplicitCollectionCreationWithConfigStepdownAtFailpoint(failpointName, collName) {
    st.configRS.awaitNodesAgreeOnPrimary();
    const configPrimary = st.configRS.getPrimary();

    clearRawMongoProgramOutput();
    setFailpoint(failpointName, configPrimary);

    const implicitCreateCode = `assert.commandWorked(db.getSiblingDB("${
        dbName}").runCommand({insert: "${collName}", documents: [{_id: 1}]}));`;
    const awaitResult = startParallelShell(implicitCreateCode, st.s.port);
    waitForFailpoint("Hit " + failpointName, 1);

    assert.commandWorked(configPrimary.adminCommand({replSetStepDown: 1, force: true}));
    unsetFailpoint(failpointName, configPrimary);

    awaitResult();
}

const dbName = "test";

const st = new ShardingTest({
    shards: 1,
    other: {
        mongosOptions: {verbose: 3},
        configOptions: {
            setParameter:
                {"failpoint.writeUnshardedCollectionsToShardingCatalog": "{mode: 'alwaysOn'}"}
        }
    }
});

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

//
// Explicit view creation
//

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log(`Check creating a view using the create command; ns: ${ns}`);

    checkNotInShardingCatalog({ns, mongosConn: st.s});
    checkNotInStorageCatalog({dbName, collName, shardConn: st.shard0});

    // Create an underlying collection for the view.
    const sourceCollName = collName + ".source";
    assert.commandWorked(st.s.getDB(dbName).runCommand({create: sourceCollName}));

    assert.commandWorked(st.s.getDB(dbName).runCommand(
        {create: collName, viewOn: sourceCollName, pipeline: [{$project: {_id: 1}}]}));

    checkNotInShardingCatalog({ns, mongosConn: st.s});
    checkInStorageCatalog({dbName, collName, type: "view", shardConn: st.shard0});
})();

//
// Explicit collection creation
//

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log(`Check creating an unsharded collection using the create command; ns: ${ns}`);

    checkNotInShardingCatalog({ns, mongosConn: st.s});
    checkNotInStorageCatalog({dbName, collName, shardConn: st.shard0});

    assert.commandWorked(st.s.getDB(dbName).runCommand({create: collName}));

    checkInShardingCatalog({
        ns: ns,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog({dbName, collName, type: "collection", shardConn: st.shard0});
})();

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log(
        `Check creating an unsharded collection that already exists with the same storage options;
        ns: ${ns}`);

    // Create the collection once.
    assert.commandWorked(st.s.getDB(dbName).runCommand({create: collName}));

    // Retrying create should succeed.
    assert.commandWorked(st.s.getDB(dbName).runCommand({create: collName}));

    checkInShardingCatalog({
        ns: ns,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog({dbName, collName, type: "collection", shardConn: st.shard0});
})();

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log(
        `Check creating an unsharded collection that already exists with different storage options;
        ns: ${ns}`);

    // Create the collection with some non-default storage option.
    assert.commandWorked(st.s.getDB(dbName).runCommand(
        {create: collName, validator: {stringField: {$type: "int"}}}));

    // Trying to create the collection with default storage options should fail.
    assert.commandFailedWithCode(st.s.getDB(dbName).runCommand({create: collName}),
                                 ErrorCodes.NamespaceExists);
})();

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log(`Check creating an unsharded collection that is already a view; ns: ${ns}`);

    // Create an underlying collection for the view.
    const sourceCollName = collName + ".source";
    assert.commandWorked(st.s.getDB(dbName).runCommand({create: sourceCollName}));

    // Create the view itself.
    assert.commandWorked(st.s.getDB(dbName).runCommand(
        {create: collName, viewOn: sourceCollName, pipeline: [{$project: {_id: 1}}]}));

    // Trying to create a collection with the same name as the view should fail.
    assert.commandFailedWithCode(st.s.getDB(dbName).runCommand({create: collName}),
                                 ErrorCodes.NamespaceExists);
})();

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log(
        `Check creating an unsharded collection that's already sharded with default sharding 
        options; ns: ${ns}`);

    // Create a sharded collection that has the default unsharded collection sharding options.
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}, unique: false}));
    checkInShardingCatalog({
        ns: ns,
        shardKey: "_id",
        unique: false,
        distributionMode: "sharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog({dbName, collName, type: "collection", shardConn: st.shard0});

    // Trying to create the collection should succeed.
    assert.commandWorked(st.s.getDB(dbName).runCommand({create: collName}));
    checkInShardingCatalog({
        ns: ns,
        shardKey: "_id",
        unique: false,
        distributionMode: "sharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog({dbName, collName, type: "collection", shardConn: st.shard0});
})();

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log(
        `Check creating an unsharded collection that's already sharded with non-default
        sharding options, ns: ${ns}`);

    // Create a sharded collection that has sharding options that are different from the default
    // unsharded collection sharding options.
    assert.commandWorked(
        st.s.getDB(dbName).adminCommand({shardCollection: ns, key: {x: 1}, unique: true}));
    checkInShardingCatalog({
        ns: ns,
        shardKey: "x",
        unique: true,
        distributionMode: "sharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog({dbName, collName, type: "collection", shardConn: st.shard0});

    // Trying to create the collection should succeed.
    assert.commandWorked(st.s.getDB(dbName).runCommand({create: collName}));
    checkInShardingCatalog({
        ns: ns,
        shardKey: "x",
        unique: true,
        distributionMode: "sharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog({dbName, collName, type: "collection", shardConn: st.shard0});
})();

//
// Implicit collection creation
//

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log(`Check creating an unsharded collection implicitly through a write; ns: ${ns}`);

    assert.commandWorked(st.s.getDB(dbName).runCommand({insert: collName, documents: [{_id: 1}]}));

    checkInShardingCatalog({
        ns: ns,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog({dbName, collName, type: "collection", shardConn: st.shard0});
})();

//
// Config failovers during *explicit* collection creation (ensures mongos retries appropriately, and
// the retry succeeds)
//

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log(`Check config stepdown during explicit create after acquiring distlocks; ns: ${ns}`);

    runExplicitCollectionCreationWithConfigStepdownAtFailpoint(
        "hangCreateCollectionAfterAcquiringDistlocks", collName);

    checkInShardingCatalog({
        ns: ns,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog({dbName, collName, type: "collection", shardConn: st.shard0});
})();

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log(
        `Check config stepdown during explicit create after sending create to primary shard;
        ns: ${ns}`);

    runExplicitCollectionCreationWithConfigStepdownAtFailpoint(
        "hangCreateCollectionAfterSendingCreateToPrimaryShard", collName);

    checkInShardingCatalog({
        ns: ns,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog({dbName, collName, type: "collection", shardConn: st.shard0});
})();

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log(
        `Check config stepdown during explicit create after getting UUID from primary shard; ns:
        ${ns}`);

    runExplicitCollectionCreationWithConfigStepdownAtFailpoint(
        "hangCreateCollectionAfterGettingUUIDFromPrimaryShard", collName);

    checkInShardingCatalog({
        ns: ns,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog({dbName, collName, type: "collection", shardConn: st.shard0});
})();

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log(
        `Check config stepdown during explicit create after writing entry to config.chunks; ns:
        ${ns}`);

    runExplicitCollectionCreationWithConfigStepdownAtFailpoint(
        "hangCreateCollectionAfterWritingEntryToConfigChunks", collName);

    checkInShardingCatalog({
        ns: ns,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog({dbName, collName, type: "collection", shardConn: st.shard0});
})();

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log(
        `Check config stepdown during explicit create after writing entry to config.collections; ns:
        ${ns}`);

    runExplicitCollectionCreationWithConfigStepdownAtFailpoint(
        "hangCreateCollectionAfterWritingEntryToConfigCollections", collName);

    checkInShardingCatalog({
        ns: ns,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog({dbName, collName, type: "collection", shardConn: st.shard0});
})();

//
// Config failovers during *implicit* collection creation (ensures the shard and mongos retry
// appropriately, and the retry succeeds)
//

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log(`Check config stepdown during implicit create after acquiring distlocks; ns: {$ns}`);

    runImplicitCollectionCreationWithConfigStepdownAtFailpoint(
        "hangCreateCollectionAfterAcquiringDistlocks", collName);

    checkInShardingCatalog({
        ns: ns,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog({dbName, collName, type: "collection", shardConn: st.shard0});
})();

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log(
        `Check config stepdown during implicit create after sending create to primary shard; ns:
        ${ns}`);

    runImplicitCollectionCreationWithConfigStepdownAtFailpoint(
        "hangCreateCollectionAfterSendingCreateToPrimaryShard", collName);

    checkInShardingCatalog({
        ns: ns,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog({dbName, collName, type: "collection", shardConn: st.shard0});
})();

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log(
        `Check config stepdown during implicit create after getting UUID from primary shard; ns:
        ${ns}`);

    runImplicitCollectionCreationWithConfigStepdownAtFailpoint(
        "hangCreateCollectionAfterGettingUUIDFromPrimaryShard", collName);

    checkInShardingCatalog({
        ns: ns,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog({dbName, collName, type: "collection", shardConn: st.shard0});
})();

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log(
        `Check config stepdown during implicit create after writing entry to config.chunks; ns:
        ${ns}`);

    runImplicitCollectionCreationWithConfigStepdownAtFailpoint(
        "hangCreateCollectionAfterWritingEntryToConfigChunks", collName);

    checkInShardingCatalog({
        ns: ns,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog({dbName, collName, type: "collection", shardConn: st.shard0});
})();

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log(
        `Check config stepdown during implicit create after writing entry to config.collections; ns:
        ${ns}`);

    runImplicitCollectionCreationWithConfigStepdownAtFailpoint(
        "hangCreateCollectionAfterWritingEntryToConfigCollections", collName);

    checkInShardingCatalog({
        ns: ns,
        shardKey: "_id",
        unique: false,
        distributionMode: "unsharded",
        numChunks: 1,
        mongosConn: st.s
    });
    checkInStorageCatalog({dbName, collName, type: "collection", shardConn: st.shard0});
})();

st.stop();
})();
