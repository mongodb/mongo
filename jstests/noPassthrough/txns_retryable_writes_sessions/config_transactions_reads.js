/* Test that snapshot reads and afterClusterTime majority reads are not allowed on
 * config.transactions.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const replSet = new ReplSetTest({nodes: 1});

replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const primaryDB = primary.getDB("config");

const operationTime = assert.commandWorked(primaryDB.runCommand({find: "transactions"})).operationTime;
assert.commandWorked(primaryDB.runCommand({find: "transactions", readConcern: {level: "majority"}}));
// When DisableTransactionUpdateCoalescing is enabled, we disable transaction update coalescing on
// secondaries to ensure that the history of updates is identical between the primary and
// secondaries. Then, snapshot and point-in-time (PIT) reads are allowed.
if (FeatureFlagUtil.isPresentAndEnabled(primary, "DisableTransactionUpdateCoalescing")) {
    assert.commandWorked(
        primaryDB.runCommand({find: "transactions", readConcern: {level: "majority", afterClusterTime: operationTime}}),
    );
    assert.commandWorked(primaryDB.runCommand({find: "transactions", readConcern: {level: "snapshot"}}));
    assert.commandWorked(
        primaryDB.runCommand({find: "transactions", readConcern: {level: "snapshot", atClusterTime: operationTime}}),
    );
} else {
    assert.commandFailedWithCode(
        primaryDB.runCommand({find: "transactions", readConcern: {level: "majority", afterClusterTime: operationTime}}),
        5557800,
    );
    assert.commandFailedWithCode(
        primaryDB.runCommand({find: "transactions", readConcern: {level: "snapshot"}}),
        5557800,
    );
    assert.commandFailedWithCode(
        primaryDB.runCommand({find: "transactions", readConcern: {level: "snapshot", atClusterTime: operationTime}}),
        5557800,
    );
}

replSet.stopSet();
