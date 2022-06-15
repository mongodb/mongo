/**
 * Tests a variety of functionality related to committed reads:
 *  - A killOp command can successfully kill an operation that is waiting for snapshots to be
 *    created.
 *  - A user should not be able to do any committed reads before a snapshot has been blessed.
 *  - Inserts and catalog changes should not be visible in a snapshot before they occurred.
 *  - A getMore should see the new blessed snapshot.
 *  - Dropping an index should bump the min snapshot version.
 *  - Dropping a collection is visible in committed snapshot, since metadata changes are special.
 *  - 'local'-only commands should error on majority-committed levels, and accept 'local' level.
 *
 * All of this requires support for committed reads, so this test will be skipped if the storage
 * engine does not support them.
 * @tags: [
 *   requires_majority_read_concern,
 * ]
 */

load("jstests/libs/analyze_plan.js");

(function() {
"use strict";

// Tests the functionality for committed reads for the given read concern level.
function testReadConcernLevel(level) {
    var replTest = new ReplSetTest({
        nodes: 1,
        oplogSize: 2,
        nodeOptions:
            {setParameter: 'testingSnapshotBehaviorInIsolation=true', enableMajorityReadConcern: ''}
    });
    replTest.startSet();
    // Cannot wait for a stable recovery timestamp with 'testingSnapshotBehaviorInIsolation'
    // set.
    replTest.initiateWithAnyNodeAsPrimary(
        null, "replSetInitiate", {doNotWaitForStableRecoveryTimestamp: true});

    const session =
        replTest.getPrimary().getDB("test").getMongo().startSession({causalConsistency: false});
    const db = session.getDatabase("test");
    const t = db.coll;

    function assertNoSnapshotAvailableForReadConcernLevel() {
        var res =
            t.runCommand('find', {batchSize: 2, readConcern: {level: level}, maxTimeMS: 1000});
        assert.commandFailed(res);
        assert.eq(res.code, ErrorCodes.MaxTimeMSExpired);
    }

    function assertNoSnapshotAvailableForReadConcernLevelByUUID(uuid) {
        var res =
            db.runCommand({find: uuid, batchSize: 2, readConcern: {level: level}, maxTimeMS: 1000});
        assert.commandFailed(res);
        assert.eq(res.code, ErrorCodes.MaxTimeMSExpired);
    }

    function getCursorForReadConcernLevel() {
        var res = t.runCommand('find', {batchSize: 2, readConcern: {level: level}});
        assert.commandWorked(res);
        return new DBCommandCursor(db, res, 2, undefined);
    }

    function getAggCursorForReadConcernLevel() {
        var res = t.runCommand('aggregate',
                               {pipeline: [], cursor: {batchSize: 2}, readConcern: {level: level}});
        assert.commandWorked(res);
        return new DBCommandCursor(db, res, 2, undefined);
    }

    function getExplainPlan(query) {
        var res = db.runCommand({explain: {find: t.getName(), filter: query}});
        return getWinningPlan(assert.commandWorked(res).queryPlanner);
    }

    //
    // Actual Test
    //

    // Ensure killOp will work on an op that is waiting for snapshots to be created
    var blockedReader = startParallelShell(
        "const session = db.getMongo().startSession({causalConsistency: false}); " +
            "const sessionDB = session.getDatabase(db.getName()); " +
            "sessionDB.coll.runCommand('find', {batchSize: 2, readConcern: {level: \"" + level +
            "\"}});",
        replTest.ports[0]);

    assert.soon(function() {
        var curOps = db.currentOp(true);
        jsTestLog("curOp output: " + tojson(curOps));
        for (var i in curOps.inprog) {
            var op = curOps.inprog[i];
            if (op.op === 'query' && op.ns === "test.$cmd" && op.command.find === 'coll') {
                db.killOp(op.opid);
                return true;
            }
        }
        return false;
    }, "could not kill an op that was waiting for a snapshot", 60 * 1000);
    blockedReader();

    var snapshot1 = assert.commandWorked(db.adminCommand("makeSnapshot")).name;
    assert.commandWorked(db.runCommand({create: "coll"}));
    var snapshot2 = assert.commandWorked(db.adminCommand("makeSnapshot")).name;

    for (var i = 0; i < 10; i++) {
        assert.commandWorked(t.insert({_id: i, version: 3}));
    }

    assertNoSnapshotAvailableForReadConcernLevel();

    var snapshot3 = assert.commandWorked(db.adminCommand("makeSnapshot")).name;

    assertNoSnapshotAvailableForReadConcernLevel();

    assert.commandWorked(t.update({}, {$set: {version: 4}}, false, true));
    var snapshot4 = assert.commandWorked(db.adminCommand("makeSnapshot")).name;

    // Collection didn't exist in snapshot 1.
    assert.commandWorked(db.adminCommand({"setCommittedSnapshot": snapshot1}));
    assertNoSnapshotAvailableForReadConcernLevel();

    // Collection existed but was empty in snapshot 2.
    assert.commandWorked(db.adminCommand({"setCommittedSnapshot": snapshot2}));
    assert.eq(getCursorForReadConcernLevel().itcount(), 0);
    assert.eq(getAggCursorForReadConcernLevel().itcount(), 0);

    // In snapshot 3 the collection was filled with {version: 3} documents.
    assert.commandWorked(db.adminCommand({"setCommittedSnapshot": snapshot3}));
    assert.eq(getAggCursorForReadConcernLevel().itcount(), 10);
    getAggCursorForReadConcernLevel().forEach(function(doc) {
        // Note: agg uses internal batching so can't reliably test flipping snapshot. However,
        // it uses the same mechanism as find, so if one works, both should.
        assert.eq(doc.version, 3);
    });

    assert.eq(getCursorForReadConcernLevel().itcount(), 10);
    var cursor = getCursorForReadConcernLevel();  // Note: uses batchsize=2.
    assert.eq(cursor.next().version, 3);
    assert.eq(cursor.next().version, 3);
    assert(!cursor.objsLeftInBatch());

    // In snapshot 4 the collection was filled with {version: 3} documents.
    assert.commandWorked(db.adminCommand({"setCommittedSnapshot": snapshot4}));

    // This triggers a getMore which sees the new version.
    assert.eq(cursor.next().version, 4);
    assert.eq(cursor.next().version, 4);

    // Adding an index does not bump the min snapshot for a collection. Collection scans are
    // possible, however the index is not guaranteed to be usable until the majority-committed
    // snapshot advances.
    t.createIndex({version: 1}, {}, 0);
    assert.eq(getCursorForReadConcernLevel().itcount(), 10);
    assert.eq(getAggCursorForReadConcernLevel().itcount(), 10);

    // To use the index, a snapshot created after the index was completed must be marked
    // committed.
    var newSnapshot = assert.commandWorked(db.adminCommand("makeSnapshot")).name;
    assert.commandWorked(db.adminCommand({"setCommittedSnapshot": newSnapshot}));
    assert.eq(getCursorForReadConcernLevel().itcount(), 10);
    assert.eq(getAggCursorForReadConcernLevel().itcount(), 10);
    assert(isIxscan(db, getExplainPlan({version: 1})));

    // Dropping an index does not bump the min snapshot, so the query should succeed.
    t.dropIndex({version: 1});
    assert.eq(getCursorForReadConcernLevel().itcount(), 10);
    assert.eq(getAggCursorForReadConcernLevel().itcount(), 10);
    assert(isCollscan(db, getExplainPlan({version: 1})));

    newSnapshot = assert.commandWorked(db.adminCommand("makeSnapshot")).name;
    assert.commandWorked(db.adminCommand({"setCommittedSnapshot": newSnapshot}));
    assert.eq(getCursorForReadConcernLevel().itcount(), 10);
    assert.eq(getAggCursorForReadConcernLevel().itcount(), 10);
    assert(isCollscan(db, getExplainPlan({version: 1})));

    // Get the UUID before renaming.
    const collUuid = (() => {
        const collectionInfos =
            assert.commandWorked(db.runCommand({listCollections: 1})).cursor.firstBatch;
        assert.eq(1, collectionInfos.length);
        const info = collectionInfos[0];
        assert.eq(t.getName(), info.name);
        return info.info.uuid;
    })();
    assert(collUuid);

    // Get a cursor before renaming.
    cursor = getCursorForReadConcernLevel();  // Note: uses batchsize=2.
    assert.eq(cursor.next().version, 4);
    assert.eq(cursor.next().version, 4);
    assert(!cursor.objsLeftInBatch());

    // Even though renaming advances the minimum visible snapshot, we're querying by a namespace
    // that no longer exists. Because of this, the query surprisingly returns no results instead of
    // timing out. This violates read-committed semantics but is allowed by the current
    // specification.
    const tempNs = db.getName() + '.temp';
    assert.commandWorked(db.adminCommand({renameCollection: t.getFullName(), to: tempNs}));
    assert.eq(getCursorForReadConcernLevel().itcount(), 0);

    // Trigger a getMore that should fail due to the rename.
    let error = assert.throws(() => {
        cursor.next();
    });
    assert.eq(error.code, ErrorCodes.QueryPlanKilled);

    // Starting a new query by UUID will block because the minimum visible timestamp is ahead of the
    // majority-committed snapshot.
    assertNoSnapshotAvailableForReadConcernLevelByUUID(collUuid);

    // Renaming back will cause queries to block again because the original namespace exists, and
    // its minimum visible timestamp is ahead of the current majority-committed snapshot.
    assert.commandWorked(db.adminCommand({renameCollection: tempNs, to: t.getFullName()}));
    assertNoSnapshotAvailableForReadConcernLevel();

    newSnapshot = assert.commandWorked(db.adminCommand("makeSnapshot")).name;
    assert.commandWorked(db.adminCommand({"setCommittedSnapshot": newSnapshot}));
    assert.eq(getCursorForReadConcernLevel().itcount(), 10);
    assert.eq(getAggCursorForReadConcernLevel().itcount(), 10);

    // Dropping the collection is visible in the committed snapshot, even though it hasn't been
    // marked committed yet. This is allowed by the current specification even though it
    // violates strict read-committed semantics since we don't guarantee them on metadata
    // operations.
    t.drop();
    assert.eq(getCursorForReadConcernLevel().itcount(), 0);
    assert.eq(getAggCursorForReadConcernLevel().itcount(), 0);

    // Creating a new collection with the same name hides the collection until that operation is
    // in the committed view.
    t.insert({_id: 0, version: 8});
    assertNoSnapshotAvailableForReadConcernLevel();
    newSnapshot = assert.commandWorked(db.adminCommand("makeSnapshot")).name;
    assertNoSnapshotAvailableForReadConcernLevel();
    assert.commandWorked(db.adminCommand({"setCommittedSnapshot": newSnapshot}));
    assert.eq(getCursorForReadConcernLevel().itcount(), 1);
    assert.eq(getAggCursorForReadConcernLevel().itcount(), 1);

    // Commands that only support read concern 'local', (such as ping) must work when it is
    // explicitly specified and fail for majority-committed read concern levels.
    assert.commandWorked(db.adminCommand({ping: 1, readConcern: {level: 'local'}}));
    var res = assert.commandFailed(db.adminCommand({ping: 1, readConcern: {level: level}}));
    assert.eq(res.code, ErrorCodes.InvalidOptions);

    // Agg $out supports majority committed reads.
    assert.commandWorked(t.runCommand(
        'aggregate', {pipeline: [{$out: 'out'}], cursor: {}, readConcern: {level: 'local'}}));
    assert.commandWorked(t.runCommand(
        'aggregate', {pipeline: [{$out: 'out'}], cursor: {}, readConcern: {level: level}}));

    replTest.stopSet();
}

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");
const supportsCommittedReads =
    assert.commandWorked(db.serverStatus()).storageEngine.supportsCommittedReads;
MongoRunner.stopMongod(conn);

if (supportsCommittedReads) {
    testReadConcernLevel("majority");
}
}());
