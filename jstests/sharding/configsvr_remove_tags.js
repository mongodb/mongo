/*
 * Test the _configsvrRemoveTags internal command.
 * @tags: [
 *   requires_fcv_50,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/retryable_writes_util.js");

function runConfigsvrRemoveTagsWithRetries(conn, ns, lsid, txnNumber) {
    var res;
    assert.soon(() => {
        res = st.configRS.getPrimary().adminCommand({
            _configsvrRemoveTags: ns,
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

let st = new ShardingTest({mongos: 1, shards: 1});

const configDB = st.s.getDB('config');

const dbName = "test";
const collName = "foo";
const anotherCollName = "bar";
const ns = dbName + "." + collName;
const anotherNs = dbName + "." + anotherCollName;

let lsid = assert.commandWorked(st.s.getDB("admin").runCommand({startSession: 1})).id;

// Create a zone
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: 'zone0'}));

assert.commandWorked(
    st.s.adminCommand({updateZoneKeyRange: ns, min: {x: 0}, max: {x: 10}, zone: 'zone0'}));
assert.commandWorked(
    st.s.adminCommand({updateZoneKeyRange: ns, min: {x: 10}, max: {x: 20}, zone: 'zone0'}));

assert.commandWorked(
    st.s.adminCommand({updateZoneKeyRange: anotherNs, min: {x: 0}, max: {x: 10}, zone: 'zone0'}));
assert.commandWorked(
    st.s.adminCommand({updateZoneKeyRange: anotherNs, min: {x: 10}, max: {x: 20}, zone: 'zone0'}));

assert.eq(2, configDB.tags.countDocuments({ns: ns}));
assert.eq(2, configDB.tags.countDocuments({ns: anotherNs}));

// Remove tags matching 'ns'
assert.commandWorked(
    runConfigsvrRemoveTagsWithRetries(st.configRS.getPrimary(), ns, lsid, NumberLong(1)));

assert.eq(0, configDB.tags.countDocuments({ns: ns}));
assert.eq(2, configDB.tags.countDocuments({ns: anotherNs}));

// Create new zones
assert.commandWorked(
    st.s.adminCommand({updateZoneKeyRange: ns, min: {x: 50}, max: {x: 60}, zone: 'zone0'}));
assert.commandWorked(
    st.s.adminCommand({updateZoneKeyRange: ns, min: {x: 70}, max: {x: 80}, zone: 'zone0'}));

assert.eq(2, configDB.tags.countDocuments({ns: ns}));

// Check that _configsvrRemoveTags with a txnNumber lesser than the previous one for this session
assert.commandFailedWithCode(
    runConfigsvrRemoveTagsWithRetries(st.configRS.getPrimary(), ns, lsid, NumberLong(0)),
    ErrorCodes.TransactionTooOld);

assert.eq(2, configDB.tags.countDocuments({ns: ns}));
assert.eq(2, configDB.tags.countDocuments({ns: anotherNs}));

st.stop();
})();
