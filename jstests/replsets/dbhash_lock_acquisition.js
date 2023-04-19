/**
 * Tests that the dbHash command acquires IS mode locks on the global, database, and collection
 * resources when reading a timestamp using the $_internalReadAtClusterTime option.
 *
 * @tags: [
 *   uses_transactions,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");
load("jstests/libs/parallelTester.js");  // for Thread

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB("test");

const session = primary.startSession({causalConsistency: false});
const sessionDB = session.getDatabase(db.getName());

// We insert a document so the dbHash command has a collection to process.
assert.commandWorked(sessionDB.mycoll.insert({}, {writeConcern: {w: "majority"}}));
const clusterTime = session.getOperationTime();

// We then start a transaction in order to be able have a catalog operation queue up behind it.
session.startTransaction();
assert.commandWorked(sessionDB.mycoll.insert({}));

const ops = db.currentOp({"lsid.id": session.getSessionId().id}).inprog;
assert.eq(
    1, ops.length, () => "Failed to find session in currentOp() output: " + tojson(db.currentOp()));
assert.eq(ops[0].locks, {
    FeatureCompatibilityVersion: "w",
    ReplicationStateTransition: "w",
    Global: "w",
    Database: "w",
    Collection: "w",
});

const threadCaptruncCmd = new Thread(function(host) {
    try {
        const conn = new Mongo(host);
        const db = conn.getDB("test");

        // We use the captrunc command as a catalog operation that requires a MODE_X lock on the
        // collection. This ensures we aren't having the dbHash command queue up behind it on a
        // database-level lock. The collection isn't capped so it'll fail with an
        // IllegalOperation error response.
        assert.commandFailedWithCode(db.runCommand({captrunc: "mycoll", n: 1}),
                                     ErrorCodes.IllegalOperation);
        return {ok: 1};
    } catch (e) {
        return {ok: 0, error: e.toString(), stack: e.stack};
    }
}, db.getMongo().host);

threadCaptruncCmd.start();

assert.soon(() => {
    const ops = db.currentOp({"command.captrunc": "mycoll", waitingForLock: true}).inprog;
    return ops.length === 1;
}, () => "Failed to find create collection in currentOp() output: " + tojson(db.currentOp()));

const threadDBHash = new Thread(function(host, clusterTime) {
    try {
        const conn = new Mongo(host);
        const db = conn.getDB("test");
        assert.commandWorked(db.runCommand({
            dbHash: 1,
            $_internalReadAtClusterTime: eval(clusterTime),
        }));
        return {ok: 1};
    } catch (e) {
        return {ok: 0, error: e.toString(), stack: e.stack};
    }
}, db.getMongo().host, tojson(clusterTime));

threadDBHash.start();

assert.soon(() => {
    const ops = db.currentOp({"command.dbHash": 1, waitingForLock: true}).inprog;
    if (ops.length === 0 || !ops[0].locks.hasOwnProperty("Collection")) {
        return false;
    }
    assert.eq(ops[0].locks, {
        FeatureCompatibilityVersion: "r",
        ReplicationStateTransition: "w",
        Global: "r",
        Database: "r",
        Collection: "r",
    });
    return true;
}, () => "Failed to find create collection in currentOp() output: " + tojson(db.currentOp()));

assert.commandWorked(session.commitTransaction_forTesting());
threadCaptruncCmd.join();
threadDBHash.join();

assert.commandWorked(threadCaptruncCmd.returnData());
assert.commandWorked(threadDBHash.returnData());

session.endSession();
rst.stopSet();
})();
