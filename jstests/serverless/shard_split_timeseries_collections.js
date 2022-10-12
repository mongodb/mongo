/**
 * Tests shard split with time-series collections.
 *
 * @tags: [
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_62
 * ]
 */

(function() {
"use strict";

load("jstests/libs/uuid_util.js");
load("jstests/serverless/libs/shard_split_test.js");

const test = new ShardSplitTest({
    recipientSetName: "recipientSet",
    recipientTagName: "recipientTag",
    quickGarbageCollection: true
});
test.addRecipientNodes();

const donorPrimary = test.donor.getPrimary();

const tenantId = "testTenantId";
const tsDB = test.tenantDB(tenantId, "tsDB");
const donorTSDB = donorPrimary.getDB(tsDB);
assert.commandWorked(donorTSDB.createCollection("tsColl", {timeseries: {timeField: "time"}}));
assert.commandWorked(donorTSDB.runCommand(
    {insert: "tsColl", documents: [{_id: 1, time: ISODate()}, {_id: 2, time: ISODate()}]}));

const operation = test.createSplitOperation([tenantId]);
assert.commandWorked(operation.commit());

operation.forget();

test.stop();
})();
