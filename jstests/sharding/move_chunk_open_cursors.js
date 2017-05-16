/**
 * Tests that cursors opened before a chunk is moved will not see the effects of the chunk
 * migration.
 */
(function() {
    "use strict";
    const dbName = "test";
    const collName = jsTest.name();
    const testNs = dbName + "." + collName;

    const nDocs = 1000 * 10;
    const st = new ShardingTest({shards: 2});
    const coll = st.s0.getDB(dbName)[collName];
    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < nDocs; i++) {
        bulk.insert({_id: i});
    }
    assert.writeOK(bulk.execute());

    // Make sure we know which shard will host the data to begin.
    st.ensurePrimaryShard(dbName, "shard0000");
    assert.commandWorked(st.admin.runCommand({enableSharding: dbName}));
    assert.commandWorked(st.admin.runCommand({shardCollection: testNs, key: {_id: 1}}));

    // Open some cursors before migrating data.
    // Ensure the cursor stage at the front of the pipeline does not buffer any data.
    assert.commandWorked(
        st.shard0.adminCommand({setParameter: 1, internalDocumentSourceCursorBatchSizeBytes: 1}));
    const getMoreBatchSize = 100;
    const aggResponse = assert.commandWorked(
        coll.runCommand({aggregate: collName, pipeline: [], cursor: {batchSize: 0}}));
    const aggCursor = new DBCommandCursor(st.s0, aggResponse, getMoreBatchSize);

    assert(st.adminCommand({split: testNs, middle: {_id: nDocs / 2}}));
    assert(st.adminCommand({moveChunk: testNs, find: {_id: nDocs - 1}, to: "shard0001"}));

    assert.eq(
        aggCursor.itcount(),
        nDocs,
        "expected agg cursor to return all matching documents, even though some have migrated");

    // Test the same behavior with the find command.
    const findResponse = assert.commandWorked(
        coll.runCommand({find: collName, filter: {}, batchSize: getMoreBatchSize}));
    const findCursor = new DBCommandCursor(st.s0, findResponse, getMoreBatchSize);
    assert(st.adminCommand({split: testNs, middle: {_id: nDocs / 4}}));
    assert(st.adminCommand({moveChunk: testNs, find: {_id: 0}, to: "shard0001"}));

    assert.eq(
        findCursor.itcount(),
        nDocs,
        "expected find cursor to return all matching documents, even though some have migrated");
    st.stop();
}());
