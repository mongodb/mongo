/**
 * This test confirms that chunks get split according to a collection specific setting as they grow
 * due to data insertion.
 */

(function() {
'use strict';
load('jstests/sharding/autosplit_include.js');
load("jstests/sharding/libs/find_chunks_util.js");
load("jstests/libs/feature_flag_util.js");  // for FeatureFlagUtil.isEnabled

let st = new ShardingTest({
    name: "auto1",
    shards: 2,
    mongos: 1,
    other: {enableAutoSplit: true},
});

// TODO SERVER-66652 remove this test after 7.0 branches out
const noMoreAutoSplitterFeatureFlag =
    FeatureFlagUtil.isEnabled(st.configRS.getPrimary().getDB('admin'), "NoMoreAutoSplitter");
if (noMoreAutoSplitterFeatureFlag) {
    jsTestLog("Skipping as featureFlagNoMoreAutosplitter is enabled");
    st.stop();
    return;
}

const fullNS = "test.foo";
const bigString = "X".repeat(1024 * 1024 / 16);  // 65 KB

assert.commandWorked(st.s0.adminCommand({enablesharding: "test"}));
st.ensurePrimaryShard('test', st.shard1.shardName);
assert.commandWorked(st.s0.adminCommand({shardcollection: fullNS, key: {num: 1}}));

let db = st.getDB("test");
let coll = db.foo;

let i = 0;

// Inserts numDocs documents into the collection, waits for any ongoing
// splits to finish, and then prints some information about the
// collection's chunks
function insertDocsAndWaitForSplit(numDocs) {
    let bulk = coll.initializeUnorderedBulkOp();
    let curMaxKey = i;
    // Increment the global 'i' variable to keep 'num' unique across all
    // documents
    for (; i < curMaxKey + numDocs; i++) {
        bulk.insert({num: i, s: bigString});
    }
    assert.commandWorked(bulk.execute());

    waitForOngoingChunkSplits(st);

    st.printChunks();
    st.printChangeLog();
}

let configDB = db.getSiblingDB('config');

jsTest.log("Testing enableAutoSplitter == false, chunkSize=unset ...");
{
    assert.commandWorked(
        st.s0.adminCommand({configureCollectionBalancing: fullNS, enableAutoSplitter: false}));

    let configColl = configDB.collections.findOne({_id: fullNS});

    // Check that noAutoSplit has been set to 'true' on the configsvr config.collections
    assert.eq(true, configColl.noAutoSplit);
    assert.eq(configColl.maxChunkSizeBytes, undefined);

    // Accumulate ~1MB of documents
    insertDocsAndWaitForSplit(16);

    // No split should have been performed
    assert.eq(
        findChunksUtil.countChunksForNs(st.config, fullNS), 1, "Number of chunks is more than one");
}

jsTest.log("Testing enableAutoSplitter == true, chunkSize=unset ...");
{
    assert.commandWorked(
        st.s0.adminCommand({configureCollectionBalancing: fullNS, enableAutoSplitter: true}));

    let configColl = configDB.collections.findOne({_id: fullNS});

    // Check that noAutoSplit has been set to 'true' on the configsvr config.collections
    assert.eq(false, configColl.noAutoSplit);
    assert.eq(configColl.maxChunkSizeBytes, undefined);

    // Add ~1MB of documents
    insertDocsAndWaitForSplit(16);

    // No split should have been performed
    assert.eq(
        findChunksUtil.countChunksForNs(st.config, fullNS), 1, "Number of chunks is more than one");
}

jsTest.log("Testing enableAutoSplitter == false, chunkSize=1 ...");
{
    assert.commandWorked(st.s0.adminCommand(
        {configureCollectionBalancing: fullNS, enableAutoSplitter: false, chunkSize: 1}));

    let configColl = configDB.collections.findOne({_id: fullNS});

    // Check that noAutoSplit has been set to 'true' on the configsvr config.collections
    assert.eq(true, configColl.noAutoSplit);
    assert.eq(configColl.maxChunkSizeBytes, 1024 * 1024);

    // Reach ~3MB of documents  total
    insertDocsAndWaitForSplit(16);

    assert.eq(16 * 3, db.foo.find().itcount());

    // No split should have been performed
    assert.eq(
        findChunksUtil.countChunksForNs(st.config, fullNS), 1, "Number of chunks is more than one");
}

jsTest.log("Testing enableAutoSplitter == true, chunkSize=10 ...");
{
    assert.commandWorked(st.s0.adminCommand(
        {configureCollectionBalancing: fullNS, enableAutoSplitter: true, chunkSize: 10}));

    let configColl = configDB.collections.findOne({_id: fullNS});

    // Check that noAutoSplit has been unset and chunkSizeBytes is set to 10 MB.
    assert.eq(configColl.noAutoSplit, false);
    assert.eq(configColl.maxChunkSizeBytes, 10 * 1024 * 1024);

    // Add ~20MB of documents
    insertDocsAndWaitForSplit(16 * 20);
    assert.gte(findChunksUtil.countChunksForNs(st.config, fullNS),
               2,
               "Number of chunks is less then 2, no split have been perfomed");
    assert.eq(16 * (2 + 1 + 20), db.foo.find().itcount());

    // Add ~10MB of documents
    insertDocsAndWaitForSplit(16 * 10);
    assert.gte(findChunksUtil.countChunksForNs(st.config, fullNS),
               3,
               "Number of chunks is less then 3, no split have been perfomed");
    assert.eq(16 * (2 + 1 + 20 + 10), db.foo.find().itcount());
}

printjson(db.stats());

st.stop();
})();
