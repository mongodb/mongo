/**
 * Functions and variables shared between multiversion/sharded_txn_upgrade_cluster.js and
 * multiversion/sharded_txn_downgrade_cluster.js.
 */

load("jstests/sharding/libs/sharded_transactions_helpers.js");

// Define autocommit as a variable so it can be used in object literals w/o an explicit value.
const autocommit = false;

// Sets up a cluster at the given binary version with two shards and a collection sharded by "skey"
// with one chunk on each shard.
function setUpTwoShardClusterWithBinVersion(dbName, collName, binVersion) {
    const st = new ShardingTest({
        shards: 2,
        other: {
            mongosOptions: {binVersion},
            configOptions: {binVersion},
            rsOptions: {binVersion},
        },
        rs: {nodes: 3}  // Use 3 node replica sets to allow binary changes with no downtime.
    });
    checkFCV(st.configRS.getPrimary().getDB("admin"),
             binVersion === "latest" ? latestFCV : lastStableFCV);

    // Set up a sharded collection with two chunks, one on each shard.
    const ns = dbName + "." + collName;
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {skey: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {skey: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {skey: 1}, to: st.shard1.shardName}));

    flushRoutersAndRefreshShardMetadata(st, {ns, dbNames: [dbName]});

    return st;
}

// Runs a transaction against the given database using the given txnId by running two inserts.
// Depending on the multiShard parameter, will insert to one or two shards. Assumes testDB is a
// sharded collection sharded by skey, with chunks: [minKey, 0), [0, maxKey).
function runTxn(testDB, collName, {lsid, txnNumber}, {multiShard}) {
    const docs = multiShard ? [{skey: -1}, {skey: 1}] : [{skey: 1}];
    const startTransactionRes = testDB.runCommand({
        insert: collName,
        documents: docs,
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        lsid,
        autocommit,
    });
    if (!startTransactionRes.ok) {
        return startTransactionRes;
    }

    const secondStatementRes = testDB.runCommand({
        insert: collName,
        documents: docs,
        txnNumber: NumberLong(txnNumber),
        lsid,
        autocommit,
    });
    if (!secondStatementRes.ok) {
        return secondStatementRes;
    }

    return testDB.adminCommand(
        {commitTransaction: 1, lsid, txnNumber: NumberLong(txnNumber), autocommit});
}

// Retries commitTransaction for the given txnId, returning the response.
function retryCommit(testDB, {lsid, txnNumber}) {
    return testDB.adminCommand(
        {commitTransaction: 1, lsid, txnNumber: NumberLong(txnNumber), autocommit});
}

// Global counter for the number of multi shard retryable writes completed. Used to verify retried
// retryable writes aren't double applied.
let numMultiShardRetryableWrites = 0;

// Runs a dummy retryable write against two shards and increments the retryable write counter.
// Assumes testDB is a sharded collection sharded by skey, with chunks: [minKey, 0), [0, maxKey).
function assertMultiShardRetryableWriteWorked(testDB, collName, {lsid, txnNumber}) {
    numMultiShardRetryableWrites += 1;
    assert.commandWorked(testDB.runCommand({
        insert: collName,
        documents: [{skey: -1, fromRetryableWrite: true}, {skey: 1, fromRetryableWrite: true}],
        txnNumber: NumberLong(txnNumber),
        lsid
    }));
}

// Verifies a txnId has already been used for a retryable write by running a dummy retryable write
// and asserting the write isn't applied. Assumes testDB is a sharded collection sharded by skey,
// with chunks: [minKey, 0), [0, maxKey).
function assertMultiShardRetryableWriteCanBeRetried(testDB, collName, {lsid, txnNumber}) {
    assert.commandWorked(testDB.runCommand({
        insert: collName,
        documents: [{skey: -1, fromRetryableWrite: true}, {skey: 1, fromRetryableWrite: true}],
        txnNumber: NumberLong(txnNumber),
        lsid
    }));
    assert.eq(numMultiShardRetryableWrites * 2,  // Each write inserts 2 documents.
              testDB[collName].find({fromRetryableWrite: true}).itcount());
}

// Recreates unique indexes in the FCV 4.0 format to allow for a binary downgrade form 4.2 to 4.0.
//
// Taken from:
// https://docs.mongodb.com/master/release-notes/4.2-downgrade-sharded-cluster/#remove-backwards-incompatible-persisted-features
function downgradeUniqueIndexesScript(db) {
    var unique_idx_v1 = [];
    var unique_idx_v2 = [];
    db.adminCommand("listDatabases").databases.forEach(function(d) {
        let mdb = db.getSiblingDB(d.name);
        mdb.getCollectionInfos().forEach(function(c) {
            let currentCollection = mdb.getCollection(c.name);
            currentCollection.getIndexes().forEach(function(spec) {
                if (!spec.unique) {
                    return;
                }

                const ns = d.name + "." + c.name;
                if (spec.v === 1) {
                    unique_idx_v1.push({ns: ns, spec: spec});
                } else {
                    unique_idx_v2.push({ns: ns, spec: spec});
                }
            });
        });
    });

    // Drop and recreate all v:1 indexes
    for (let pair of unique_idx_v1) {
        const ns = pair.ns;
        const idx = pair.spec;
        let [dbName, collName] = ns.split(".");
        let res = db.getSiblingDB(dbName).runCommand({dropIndexes: collName, index: idx.name});
        assert.commandWorked(res);
        res = db.getSiblingDB(dbName).runCommand({
            createIndexes: collName,
            indexes: [{"key": idx.key, "name": idx.name, "unique": true, "v": 1}]
        });
        assert.commandWorked(res);
    }

    // Drop and recreate all v:2 indexes
    for (let pair of unique_idx_v2) {
        const ns = pair.ns;
        const idx = pair.spec;
        let [dbName, collName] = ns.split(".");
        let res = db.getSiblingDB(dbName).runCommand({dropIndexes: collName, index: idx.name});
        assert.commandWorked(res);
        res = db.getSiblingDB(dbName).runCommand({
            createIndexes: collName,
            indexes: [{"key": idx.key, "name": idx.name, "unique": true, "v": 2}]
        });
        assert.commandWorked(res);
    }
}
