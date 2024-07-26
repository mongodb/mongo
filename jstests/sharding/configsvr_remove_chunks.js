/*
 * Test the _configsvrRemoveChunks internal command.
 * @tags: [
 *   requires_fcv_50,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/retryable_writes_util.js");
load("jstests/sharding/libs/find_chunks_util.js");

function runConfigsvrRemoveChunksWithRetries(conn, uuid, lsid, txnNumber) {
    var res;
    assert.soon(() => {
        res = st.configRS.getPrimary().adminCommand({
            _configsvrRemoveChunks: 1,
            collectionUUID: uuid,
            lsid: lsid,
            txnNumber: txnNumber,
            writeConcern: {w: "majority"}
        });

        if (RetryableWritesUtil.isRetryableCode(res.code) ||
            RetryableWritesUtil.errmsgContainsRetryableCodeName(res.errmsg) ||
            (res.writeConcernError &&
             RetryableWritesUtil.isRetryableCode(res.writeConcernError.code))) {
            return false;  // Retry
        }

        return true;
    });

    return res;
}

function insertLeftoverChunks(sessionConfigDB, configDB, uuid) {
    const chunkDocsForNs = findChunksUtil.findChunksByNs(configDB, ns).toArray();
    var chunksToInsert = [];
    chunkDocsForNs.forEach(originalChunk => {
        var newChunk = originalChunk;
        newChunk._id = ObjectId();
        newChunk.uuid = otherCollectionUUID;
        chunksToInsert.push(newChunk);
    });
    assert.commandWorked(sessionConfigDB.getCollection("chunks").insertMany(chunksToInsert));
}

let st = new ShardingTest({mongos: 1, shards: 1});

// Use retriable writes when writing to the config server since these are not automatically retried
const mongosSession = st.s.startSession({retryWrites: true});
// In v5.0, we cannot run exhaustive find commands under a session. So we use the session for write
// commands and the database without the session for read commands.
const sessionConfigDB = mongosSession.getDatabase("config");
const configDB = st.s.getDB("config");

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

let lsid = assert.commandWorked(st.s.getDB("admin").runCommand({startSession: 1})).id;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardcollection: ns, key: {_id: 1}}));

// Insert some chunks not associated to any collection to simulate leftovers from a failed
// shardCollection
const otherCollectionUUID = UUID();
insertLeftoverChunks(sessionConfigDB, configDB, otherCollectionUUID);
assert.eq(1, configDB.getCollection("chunks").find({uuid: otherCollectionUUID}).itcount());

// Remove the leftover chunks matching 'otherCollectionUUID'
assert.commandWorked(runConfigsvrRemoveChunksWithRetries(
    st.configRS.getPrimary(), otherCollectionUUID, lsid, NumberLong(1)));

assert.eq(0, configDB.getCollection("chunks").find({uuid: otherCollectionUUID}).itcount());

// Insert new leftover chunks
insertLeftoverChunks(sessionConfigDB, configDB, otherCollectionUUID);
assert.eq(1, configDB.getCollection("chunks").find({uuid: otherCollectionUUID}).itcount());

// Check that _configsvrRemoveChunks with a txnNumber lesser than the previous one won't remove the
// chunk documents
assert.commandFailedWithCode(
    runConfigsvrRemoveChunksWithRetries(
        st.configRS.getPrimary(), otherCollectionUUID, lsid, NumberLong(0)),
    ErrorCodes.TransactionTooOld);

assert.eq(1, configDB.getCollection("chunks").find({uuid: otherCollectionUUID}).itcount());

// Cleanup the leftover chunks before finishing
sessionConfigDB.getCollection("chunks").remove({uuid: otherCollectionUUID});

st.stop();
})();
