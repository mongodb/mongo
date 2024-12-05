/**
 * Tests that crashing initial sync while bulk building ready secondary indexes during the
 * collection clone phase is recoverable.
 *
 * @tags: [requires_replication]
 */

import {kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Disallow elections on secondary.
const rst = new ReplSetTest({
    nodes: [
        {},
        {
            rsConfig: {
                priority: 0,
            },
        },
        {
            rsConfig: {
                priority: 0,
            },
        }
    ]
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const dbName = "test";
const collName = jsTestName();

const db = primary.getDB(dbName);
const coll = db.getCollection(collName);

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.insert({a: 1}));

rst.awaitReplication();

// Forcefully re-sync the secondary and hang during the collection clone phase.
let secondary = rst.restart(1, {
    startClean: true,
    setParameter: {
        'failpoint.initialSyncHangDuringCollectionClone': tojson(
            {mode: 'alwaysOn', data: {namespace: dbName + "." + collName, numDocsToClone: 0}})
    }
});

// Wait until we block on cloning the collection. The index builders are initialized at this point.
assert.commandWorked(secondary.adminCommand({
    waitForFailPoint: "initialSyncHangDuringCollectionClone",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

// Take a checkpoint. This is necessary in order for the index entries to be durably written in the
// catalog with 'ready: false' before crashing the node.
assert.commandWorked(secondary.getDB("admin").runCommand({fsync: 1}));

// Crash the secondary node.
const SIGKILL = 9;
secondary = rst.restart(
    1,
    {
        startClean: false,
        allowedExitCode: MongoRunner.EXIT_SIGKILL,
        setParameter: {'failpoint.initialSyncHangDuringCollectionClone': tojson({mode: 'off'})}
    },
    SIGKILL);

// Wait for initial sync to finish.
rst.awaitSecondaryNodes(null, [secondary]);

rst.stopSet();
