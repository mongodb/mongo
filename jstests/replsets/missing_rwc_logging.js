/**
 * SERVER-44539: pin the "missing RWC" log lines on a shard-role mongod so a
 * future re-enable cannot silently regress.
 *
 * Two logv2 ids gate this: 21954 (missing readConcern) and 21959 (missing
 * writeConcern), both currently disabled in service_entry_point_shard_role.cpp
 * (search for "TODO(SERVER-44539)"). The test:
 *
 *   1. Starts a 1-node ReplSetTest with --shardsvr so the
 *      ClusterRole::ShardServer branch in _extractReadConcern /
 *      _validateAndSetWriteConcern is reachable.
 *   2. Drives an external (non-internalClient) command that omits both RWC
 *      fields directly at the shard mongod.
 *   3. Asserts each log line is present once the C++ LOGV2 calls are
 *      uncommented; while they remain commented the assertions are wrapped in
 *      assert.throws so the test stays green and flips to a real signal the
 *      moment the lines are restored.
 *
 * @tags: [requires_replication, requires_sharding]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kMissingReadConcernId = 21954;
const kMissingWriteConcernId = 21959;
const kTimeoutMs = 5 * 1000;

const rst = new ReplSetTest({nodes: 1, nodeOptions: {shardsvr: ""}});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const adminDb = primary.getDB("admin");
const testDb = primary.getDB("missing_rwc_logging");

// Drive an external command with no readConcern and no writeConcern. The
// shard-role SEP should LOGV2 both 21954 and 21959 once re-enabled. We use
// runCommand directly to avoid the driver injecting any defaults.
assert.commandWorked(testDb.runCommand({insert: "c", documents: [{_id: 1}]}));
assert.commandWorked(testDb.runCommand({find: "c", filter: {}}));

// Phase A: while the LOGV2 calls remain commented out the lines must NOT
// appear. checkLog.containsJson throws if the line is not seen within the
// timeout, so assert.throws is the green signal.
const expectMissing = (id) =>
    assert.throws(() => checkLog.containsJson(primary, id, {}, kTimeoutMs),
                  [],
                  "logv2 id " + id + " should be absent until SERVER-44539 re-enables it");

// Phase B: once SERVER-44539 restores the LOGV2 calls, swap the two
// expectMissing(...) calls below for the checkLog.containsJson(...) variants
// already noted in the comment. The diff is intentionally one-line-per-id so
// the re-enable PR can be reviewed mechanically.
expectMissing(kMissingReadConcernId);
// checkLog.containsJson(primary, kMissingReadConcernId, {command: "find"});

expectMissing(kMissingWriteConcernId);
// checkLog.containsJson(primary, kMissingWriteConcernId, {command: "insert"});

rst.stopSet();
