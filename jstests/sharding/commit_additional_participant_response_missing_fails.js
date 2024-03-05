/**
 * Tests that if a shard that added additional participants gets either a commit or prepare request
 * before it received a response from every participant that it added, it will cause the transaction
 * to be aborted.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

(function() {
'use strict';
const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

let st = new ShardingTest({shards: 3, causallyConsistent: true});

// TODO SERVER-85353 Remove or modify this test to avoid relying on the failpoint and feature
// flag to inject added participants
const featureFlagAllowAdditionalParticipants = FeatureFlagUtil.isEnabled(
    st.configRS.getPrimary().getDB('admin'), "AllowAdditionalParticipants");
if (!featureFlagAllowAdditionalParticipants) {
    jsTestLog("Skipping as featureFlagAllowAdditionalParticipants is  not enabled");
    st.stop();
    return;
}

const session = st.s.startSession();
const sessionDB = session.getDatabase(dbName);

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 10}, to: st.shard2.shardName}));

assert.commandWorked(st.shard0.adminCommand({_flushRoutingTableCacheUpdates: ns}));
assert.commandWorked(st.shard1.adminCommand({_flushRoutingTableCacheUpdates: ns}));
assert.commandWorked(st.shard2.adminCommand({_flushRoutingTableCacheUpdates: ns}));
st.refreshCatalogCacheForNs(st.s, ns);

// Insert a doc on shard0 to create the collection
assert.commandWorked(sessionDB.foo.insert({_id: -1}));

// Execute a transaction with a single participant, so that it will choose to send commit directly
// to the shard
session.startTransaction();
assert.eq(sessionDB.foo.find({_id: -1}).itcount(), 1);

// This failpoint should cause shard0 to fail the commit, because it will mock that shard0 actually
// added shard1 as an additional participant who did not yet respond to shard0.
let fpData = {"cmdName": "commitTransaction", "ns": "admin", "shardId": [st.shard1.shardName]};
let commitFp =
    configureFailPoint(st.shard0, "includeAdditionalParticipantInResponse", fpData, "alwaysOn");
assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.IllegalOperation);
commitFp.off();

// Execute a transaction with two write participants so that the router will choose to execute the
// 2PC path.
session.startTransaction();
assert.commandWorked(sessionDB.foo.insert({_id: -5}));
assert.commandWorked(sessionDB.foo.insert({_id: 5}));

// This failpoint should cause shard0 to fail the prepare request, because it will mock that shard0
// actually added shard2 as an additional participant who did not yet respond to shard0. Note that
// prepareTransaction retries until it gets an error of type "VoteAbortError". The first
// prepareTransaction request will return IllegalOperation and will cause the transaction to be
// aborted on shard0. When the coordinator retries the prepare, shard0 will then return
// NoSuchTransaction because it has already aborted the transaction.
fpData = {
    "cmdName": "prepareTransaction",
    "ns": "admin",
    "shardId": [st.shard2.shardName]
};
let prepareFp =
    configureFailPoint(st.shard0, "includeAdditionalParticipantInResponse", fpData, "alwaysOn");
assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
prepareFp.off();

st.stop();
})();