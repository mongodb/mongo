/**
 * Tests a variety of functionality related to committed reads:
 *  - A killOp command can successfully kill an operation that is waiting for snapshots to be
 *    created.
 *  - A user should not be able to do any committed reads before a snapshot has been blessed.
 *  - Inserts and catalog changes should not be visible in a snapshot before they occurred.
 *  - A getMore should see the new blessed snapshot.
 *  - Dropping an index, repairing, and reIndexing should bump the min snapshot version.
 *  - Dropping a collection is visible in committed snapshot, since metadata changes are special.
 *  - 'local'-only commands should error on 'majority' level, and accept 'local' level.
 *  - An aggregation with '$out' should fail with 'majority' level.
 *
 * All of this requires support for committed reads, so this test will be skipped if the storage
 * engine does not support them.
 */

load("jstests/libs/analyze_plan.js");

(function() {
    "use strict";

    // This test needs its own mongod since the snapshot names must be in increasing order and once
    // you
    // have a majority commit point it is impossible to go back to not having one.
    var testServer =
        MongoRunner.runMongod({setParameter: 'testingSnapshotBehaviorInIsolation=true'});
    var db = testServer.getDB("test");
    var t = db.readMajority;

    function assertNoReadMajoritySnapshotAvailable() {
        var res =
            t.runCommand('find', {batchSize: 2, readConcern: {level: "majority"}, maxTimeMS: 1000});
        assert.commandFailed(res);
        assert.eq(res.code, ErrorCodes.ExceededTimeLimit);
    }

    function getReadMajorityCursor() {
        var res = t.runCommand('find', {batchSize: 2, readConcern: {level: "majority"}});
        assert.commandWorked(res);
        return new DBCommandCursor(db.getMongo(), res, 2);
    }

    function getReadMajorityAggCursor() {
        var res = t.runCommand(
            'aggregate', {pipeline: [], cursor: {batchSize: 2}, readConcern: {level: "majority"}});
        assert.commandWorked(res);
        return new DBCommandCursor(db.getMongo(), res, 2);
    }

    function getExplainPlan(query) {
        var res = db.runCommand({explain: {find: t.getName(), filter: query}});
        return assert.commandWorked(res).queryPlanner.winningPlan;
    }

    //
    // Actual Test
    //

    if (!db.serverStatus().storageEngine.supportsCommittedReads) {
        print("Skipping read_majority.js since storageEngine doesn't support it.");
        return;
    }

    // Ensure killOp will work on an op that is waiting for snapshots to be created
    var blockedReader = startParallelShell(
        "db.readMajority.runCommand('find', {batchSize: 2, readConcern: {level: 'majority'}});",
        testServer.port);

    assert.soon(function() {
        var curOps = db.currentOp(true);
        jsTestLog("curOp output: " + tojson(curOps));
        for (var i in curOps.inprog) {
            var op = curOps.inprog[i];
            if (op.op === 'query' && op.ns === "test.$cmd" && op.query.find === 'readMajority') {
                db.killOp(op.opid);
                return true;
            }
        }
        return false;
    }, "could not kill an op that was waiting for a snapshot", 60 * 1000);
    blockedReader();

    var snapshot1 = assert.commandWorked(db.adminCommand("makeSnapshot")).name;
    assert.commandWorked(db.runCommand({create: "readMajority"}));
    var snapshot2 = assert.commandWorked(db.adminCommand("makeSnapshot")).name;

    for (var i = 0; i < 10; i++) {
        assert.writeOK(t.insert({_id: i, version: 3}));
    }

    assertNoReadMajoritySnapshotAvailable();

    var snapshot3 = assert.commandWorked(db.adminCommand("makeSnapshot")).name;

    assertNoReadMajoritySnapshotAvailable();

    assert.writeOK(t.update({}, {$set: {version: 4}}, false, true));
    var snapshot4 = assert.commandWorked(db.adminCommand("makeSnapshot")).name;

    // Collection didn't exist in snapshot 1.
    assert.commandWorked(db.adminCommand({"setCommittedSnapshot": snapshot1}));
    assertNoReadMajoritySnapshotAvailable();

    // Collection existed but was empty in snapshot 2.
    assert.commandWorked(db.adminCommand({"setCommittedSnapshot": snapshot2}));
    assert.eq(getReadMajorityCursor().itcount(), 0);
    assert.eq(getReadMajorityAggCursor().itcount(), 0);

    // In snapshot 3 the collection was filled with {version: 3} documents.
    assert.commandWorked(db.adminCommand({"setCommittedSnapshot": snapshot3}));
    assert.eq(getReadMajorityAggCursor().itcount(), 10);
    getReadMajorityAggCursor().forEach(function(doc) {
        // Note: agg uses internal batching so can't reliably test flipping snapshot. However, it
        // uses
        // the same mechanism as find, so if one works, both should.
        assert.eq(doc.version, 3);
    });

    assert.eq(getReadMajorityCursor().itcount(), 10);
    var cursor = getReadMajorityCursor();  // Note: uses batchsize=2.
    assert.eq(cursor.next().version, 3);
    assert.eq(cursor.next().version, 3);
    assert(!cursor.objsLeftInBatch());

    // In snapshot 4 the collection was filled with {version: 3} documents.
    assert.commandWorked(db.adminCommand({"setCommittedSnapshot": snapshot4}));

    // This triggers a getMore which sees the new version.
    assert.eq(cursor.next().version, 4);
    assert.eq(cursor.next().version, 4);

    // Adding an index bumps the min snapshot for a collection as of SERVER-20260. This may change
    // to
    // just filter that index out from query planning as part of SERVER-20439.
    t.ensureIndex({version: 1});
    assertNoReadMajoritySnapshotAvailable();

    // To use the index, a snapshot created after the index was completed must be marked committed.
    var newSnapshot = assert.commandWorked(db.adminCommand("makeSnapshot")).name;
    assertNoReadMajoritySnapshotAvailable();
    assert.commandWorked(db.adminCommand({"setCommittedSnapshot": newSnapshot}));
    assert.eq(getReadMajorityCursor().itcount(), 10);
    assert.eq(getReadMajorityAggCursor().itcount(), 10);
    assert(isIxscan(getExplainPlan({version: 1})));

    // Dropping an index does bump the min snapshot.
    t.dropIndex({version: 1});
    assertNoReadMajoritySnapshotAvailable();

    // To use the collection again, a snapshot created after the dropIndex must be marked committed.
    newSnapshot = assert.commandWorked(db.adminCommand("makeSnapshot")).name;
    assertNoReadMajoritySnapshotAvailable();
    assert.commandWorked(db.adminCommand({"setCommittedSnapshot": newSnapshot}));
    assert.eq(getReadMajorityCursor().itcount(), 10);

    // Reindex bumps the min snapshot.
    t.reIndex();
    assertNoReadMajoritySnapshotAvailable();
    newSnapshot = assert.commandWorked(db.adminCommand("makeSnapshot")).name;
    assertNoReadMajoritySnapshotAvailable();
    assert.commandWorked(db.adminCommand({"setCommittedSnapshot": newSnapshot}));
    assert.eq(getReadMajorityCursor().itcount(), 10);

    // Repair bumps the min snapshot.
    db.repairDatabase();
    assertNoReadMajoritySnapshotAvailable();
    newSnapshot = assert.commandWorked(db.adminCommand("makeSnapshot")).name;
    assertNoReadMajoritySnapshotAvailable();
    assert.commandWorked(db.adminCommand({"setCommittedSnapshot": newSnapshot}));
    assert.eq(getReadMajorityCursor().itcount(), 10);
    assert.eq(getReadMajorityAggCursor().itcount(), 10);

    // Dropping the collection is visible in the committed snapshot, even though it hasn't been
    // marked
    // committed yet. This is allowed by the current specification even though it violates strict
    // read-committed semantics since we don't guarantee them on metadata operations.
    t.drop();
    assert.eq(getReadMajorityCursor().itcount(), 0);
    assert.eq(getReadMajorityAggCursor().itcount(), 0);

    // Creating a new collection with the same name hides the collection until that operation is in
    // the
    // committed view.
    t.insert({_id: 0, version: 8});
    assertNoReadMajoritySnapshotAvailable();
    newSnapshot = assert.commandWorked(db.adminCommand("makeSnapshot")).name;
    assertNoReadMajoritySnapshotAvailable();
    assert.commandWorked(db.adminCommand({"setCommittedSnapshot": newSnapshot}));
    assert.eq(getReadMajorityCursor().itcount(), 1);
    assert.eq(getReadMajorityAggCursor().itcount(), 1);

    // Commands that only support read concern 'local', (such as ping) must work when it is
    // explicitly
    // specified and fail when 'majority' is specified.
    assert.commandWorked(db.adminCommand({ping: 1, readConcern: {level: 'local'}}));
    var res = assert.commandFailed(db.adminCommand({ping: 1, readConcern: {level: 'majority'}}));
    assert.eq(res.code, ErrorCodes.InvalidOptions);

    // Agg $out also doesn't support read concern majority.
    assert.commandWorked(
        t.runCommand('aggregate', {pipeline: [{$out: 'out'}], readConcern: {level: 'local'}}));
    var res = assert.commandFailed(
        t.runCommand('aggregate', {pipeline: [{$out: 'out'}], readConcern: {level: 'majority'}}));
    assert.eq(res.code, ErrorCodes.InvalidOptions);

    MongoRunner.stopMongod(testServer);
}());
