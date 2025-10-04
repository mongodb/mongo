/**
 * Tests that a server containing a TTL index with NaN for 'expireAfterSeconds'
 * will log a warning on startup.
 *
 * @tags: [
 *     requires_persistence,
 *     requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: [{}, {rsConfig: {votes: 0, priority: 0}}]});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const db = primary.getDB("test");
const coll = db.t;

// The test cases here revolve around having a TTL index in the catalog with a NaN
// 'expireAfterSeconds'. The current createIndexes behavior will overwrite NaN with int32::max
// unless we use a fail point.
const fp = configureFailPoint(primary, "skipTTLIndexValidationOnCreateIndex");
const fp2 = configureFailPoint(primary, "skipTTLIndexExpireAfterSecondsValidation");
try {
    assert.commandWorked(coll.createIndex({t: 1}, {expireAfterSeconds: NaN}));
} finally {
    fp.off();
    fp2.off();
}

assert.commandWorked(coll.insert({_id: 0, t: ISODate()}));

// Force checkpoint in storage engine to ensure index is part of the catalog in
// in finished state at startup.
rst.awaitReplication();
let secondary = rst.getSecondary();
assert.commandWorked(secondary.adminCommand({fsync: 1}));

// Restart the secondary and check for the startup warning in the logs.
secondary = rst.restart(secondary);
rst.awaitSecondaryNodes(null, [secondary]);

// Wait for "Found an existing TTL index with NaN 'expireAfterSeconds' in the catalog" log message.
checkLog.containsJson(secondary, 6852200, {
    ns: coll.getFullName(),
    spec: (spec) => {
        jsTestLog("TTL index on secondary at startup: " + tojson(spec));
        return isNaN(spec.expireAfterSeconds);
    },
});

rst.stopSet();
