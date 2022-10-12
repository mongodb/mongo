/**
 * Prove that shard split commands are not supported outside of serverless mode.
 * @tags: [requires_fcv_62, serverless]
 */

(function() {
"use strict";

const conn = MongoRunner.runMongod();
const migrationId = UUID();
const tenantIds = ["tenantA", "tenantB"];
const recipientSetName = "recipient";
const recipientTagName = "recipientNode";
assert.commandFailedWithCode(
    conn.adminCommand(
        {commitShardSplit: 1, migrationId, tenantIds, recipientSetName, recipientTagName}),
    ErrorCodes.CommandNotSupported);
assert.commandFailedWithCode(conn.adminCommand({abortShardSplit: 1, migrationId}),
                             ErrorCodes.CommandNotSupported);
assert.commandFailedWithCode(conn.adminCommand({forgetShardSplit: 1, migrationId}),
                             ErrorCodes.CommandNotSupported);
assert(!checkLog.checkContainsOnce(conn, "ShardSplitDonorService"),
       "Expected no mention of ShardSplitDonorService in logs");
MongoRunner.stopMongod(conn);
})();
