/**
 * Tests initial sync while recreating a time-series collection in the place of a dropped regular
 * collection.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "test";
const collName = jsTestName();

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
            },
        },
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
            },
        }
    ]
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const db = primary.getDB(dbName);
const coll = db.getCollection(collName);

rst.awaitReplication();

// Forcefully re-sync the secondary.
let secondary = rst.restart(1, {
    startClean: true,
    setParameter: {
        'failpoint.initialSyncHangBeforeCopyingDatabases': tojson({mode: 'alwaysOn'}),
        'numInitialSyncAttempts': 1
    }
});

// Wait until we block on cloning the collection.
checkLog.containsJson(secondary, 21179);

// Create a non-timeseries collection.
assert.commandWorked(db.createCollection(collName));

// Perform an insert and DDL operation on the non-timeseries collection.
assert.commandWorked(coll.insert({_id: 1}, {writeConcern: {w: "majority"}}));
assert.commandWorked(db.runCommand({collMod: collName, validationLevel: "moderate"}));

// Drop the non-timeseries collection.
assert(coll.drop());

// Recreate the collection as timeseries.
assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: "time"}}));

// Finish the collection cloning phase on the initial syncing node.
assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: "initialSyncHangBeforeCopyingDatabases", mode: "off"}));

rst.waitForState(secondary, ReplSetTest.State.SECONDARY);

rst.stopSet();
