/**
 * Test to ensure that two phase drop behavior for databases on replica sets works properly.
 *
 * Uses a 3 node replica set with one arbiter to verify both phases of a 2-phase database drop:
 * the 'Collections' and 'Database' phase. Executing a 'dropDatabase' command should put that
 * database into a drop-pending state. In this state, all new collection creation requests will
 * be rejected with an error with the code ErrorCodes.DatabaseDropPending. We will exit the
 * 'Collections' phase once the last collection drop has been propagated to a majority. All
 * collections in the database will be physically dropped at this point.
 *
 * During the 'Database' phase, collection creation is still disallowed. This phase removes the
 * metadata for the database from the server and appends the 'dropDatabase' operation to the oplog.
 * Unlike the 'Collections' phase, we do not wait for the 'dropDatabase' to propagate to a majority
 * unless explicitly requested by the user with a write concern.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

// Returns a list of all collections in a given database. Use 'args' as the
// 'listCollections' command arguments.
function listCollections(database, args) {
    var args = args || {};
    let failMsg = "'listCollections' command failed";
    let res = assert.commandWorked(database.runCommand("listCollections", args), failMsg);
    return res.cursor.firstBatch;
}

// Returns a list of all collection names in a given database.
function listCollectionNames(database, args) {
    return listCollections(database, args).map((c) => c.name);
}

let dbNameToDrop = "dbToDrop";
let replTest = new ReplSetTest({nodes: [{}, {}, {arbiter: true}]});

// Initiate the replica set.
replTest.startSet();
replTest.initiate();
replTest.awaitReplication();

let primary = replTest.getPrimary();
let secondary = replTest.getSecondary();
// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

let dbToDrop = primary.getDB(dbNameToDrop);
let collNameToDrop = "collectionToDrop";

// Create the collection that will be dropped and let it replicate.
let collToDrop = dbToDrop.getCollection(collNameToDrop);
assert.commandWorked(collToDrop.insert({_id: 0}, {writeConcern: {w: 2, wtimeout: replTest.timeoutMS}}));
assert.eq(1, collToDrop.find().itcount());

// Pause the oplog fetcher on secondary so that commit point doesn't advance, meaning that a dropped
// database on the primary will remain in 'drop-pending' state. As there isn't anything in the oplog
// buffer at this time, it is safe to pause the oplog fetcher.
jsTestLog("Pausing the oplog fetcher on the secondary node.");
stopServerReplication(secondary);

// Make sure the collection was created.
assert.contains(
    collNameToDrop,
    listCollectionNames(dbToDrop),
    "Collection '" + collNameToDrop + "' wasn't created properly",
);

/**
 * DROP DATABASE 'Collections' PHASE
 */

// Drop the collection on the primary.
let dropDatabaseFn = function () {
    let dbNameToDrop = "dbToDrop";
    let primary = db.getMongo();
    jsTestLog(
        "Dropping database " +
            dbNameToDrop +
            " on primary node " +
            primary.host +
            ". This command will block because the oplog fetcher is paused on the secondary.",
    );
    let dbToDrop = db.getSiblingDB(dbNameToDrop);
    assert.commandWorked(dbToDrop.dropDatabase());
    jsTestLog("Database " + dbNameToDrop + " successfully dropped on primary node " + primary.host);
};
let dropDatabaseProcess = startParallelShell(dropDatabaseFn, primary.port);

// Check that primary has started two phase drop of the collection.
jsTestLog(
    "Waiting for primary " + primary.host + " to prepare two phase drop of collection " + collToDrop.getFullName(),
);
assert.soonNoExcept(
    function () {
        return collToDrop.find().itcount() == 0;
    },
    "Primary " + primary.host + " failed to prepare two phase drop of collection " + collToDrop.getFullName(),
);

// Commands that manipulate the database being dropped or perform destructive catalog operations
// should fail with the DatabaseDropPending error code while the database is in a drop-pending
// state.
assert.commandFailedWithCode(
    dbToDrop.createCollection("collectionToCreateWhileDroppingDatabase"),
    ErrorCodes.DatabaseDropPending,
    "collection creation should fail while we are in the process of dropping the database",
);

/**
 * DROP DATABASE 'Database' PHASE
 */

// Let the secondary apply the collection drop operation, so that the replica set commit point
// will advance, and the 'Database' phase of the database drop will complete on the primary.
jsTestLog("Restarting the oplog fetcher on the secondary node.");
restartServerReplication(secondary);

jsTestLog("Waiting for collection drop operation to replicate to all nodes.");
replTest.awaitReplication();

jsTestLog("Waiting for dropDatabase command on " + primary.host + " to complete.");
let exitCode = dropDatabaseProcess();

let db = primary.getDB(dbNameToDrop);
checkLog.contains(
    db.getMongo(),
    `dropDatabase - dropping collection","attr":{"db":"${
        dbNameToDrop
    }","namespace":"${dbNameToDrop}.${collNameToDrop}"`,
);
checkLog.containsJson(db.getMongo(), 20336, {"db": "dbToDrop"});

assert.eq(0, exitCode, "dropDatabase command on " + primary.host + " failed.");
jsTestLog("Completed dropDatabase command on " + primary.host);

replTest.stopSet();
