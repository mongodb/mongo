/**
 * Tests whether an initial syncing node can apply a rename collection oplog entry without crashing
 * when an index build is active on the target collection namespace (nss).
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_build.js";

const dbName = "test";
const collName = "coll";

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
const dbColl = db[collName];

assert.commandWorked(dbColl.insert([{_id: 1, a: 1}, {_id: 2, a: 2}, {_id: 3, a: 3}]));

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

assert.commandWorked(dbColl.renameCollection("anotherColl"));

const createIdx = IndexBuildTest.startIndexBuild(primary, "test.anotherColl", {a: 1});

// Finish the collection cloning phase on the initial syncing node.
assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: "initialSyncHangBeforeCopyingDatabases", mode: "off"}));

rst.awaitSecondaryNodes(null, [secondary]);
createIdx();

rst.stopSet();
