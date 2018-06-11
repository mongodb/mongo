/**
 * Tests for the aggregate command's support for readConcern level "snapshot".
 *
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";

    const kAdminDB = "admin";
    const kCollName = "coll";
    const kConfigDB = "config";
    const kDBName = "test";
    const kIllegalStageForSnapshotReadCode = 50742;
    const kWCMajority = {writeConcern: {w: "majority"}};

    let rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    let session =
        rst.getPrimary().getDB(kDBName).getMongo().startSession({causalConsistency: false});
    let sessionDB = session.getDatabase(kDBName);

    let txnNumber = NumberLong(0);
    assert.commandWorked(sessionDB.runCommand({create: kCollName, writeConcern: {w: "majority"}}));

    function testSnapshotAggFailsWithCode(coll, pipeline, code) {
        let cmd = {aggregate: coll, pipeline: pipeline, cursor: {}};

        let cmdAsSnapshotRead = Object.extend({}, cmd);
        cmdAsSnapshotRead.txnNumber = NumberLong(++txnNumber);
        cmdAsSnapshotRead.readConcern = {level: "snapshot"};
        cmdAsSnapshotRead.autocommit = false;
        cmdAsSnapshotRead.startTransaction = true;
        assert.commandFailedWithCode(sessionDB.runCommand(cmdAsSnapshotRead), code);

        // As a sanity check, also make sure that the command succeeds when run without a txn number
        // and without a readConcern.
        assert.commandWorked(sessionDB.runCommand(cmd));
    }

    // Test that $changeStream is disallowed with snapshot reads.
    testSnapshotAggFailsWithCode(kCollName, [{$changeStream: {}}], ErrorCodes.InvalidOptions);

    // Test that $collStats is disallowed with snapshot reads.
    testSnapshotAggFailsWithCode(kCollName, [{$collStats: {}}], kIllegalStageForSnapshotReadCode);

    // Test that $indexStats is disallowed with snapshot reads.
    testSnapshotAggFailsWithCode(kCollName, [{$indexStats: {}}], kIllegalStageForSnapshotReadCode);

    // Test that $listLocalCursors is disallowed with snapshot reads.
    testSnapshotAggFailsWithCode(1, [{$listLocalCursors: {}}], ErrorCodes.InvalidOptions);

    // Test that $listLocalSessions is disallowed with snapshot reads.
    testSnapshotAggFailsWithCode(1, [{$listLocalSessions: {}}], ErrorCodes.InvalidOptions);

    // Test that $out is disallowed with snapshot reads.
    testSnapshotAggFailsWithCode(kCollName, [{$out: "out"}], ErrorCodes.InvalidOptions);

    // Test that $listSessions is disallowed with snapshot reads. This stage must be run against
    // 'system.sessions' in the config database, which cannot be queried in a transaction.
    sessionDB = session.getDatabase(kConfigDB);
    testSnapshotAggFailsWithCode("system.sessions", [{$listSessions: {}}], 50844);

    // Test that $currentOp is disallowed with snapshot reads. We have to reassign 'sessionDB' to
    // refer to the admin database, because $currentOp pipelines are required to run against
    // 'admin'. Queries against 'admin' are not permitted in a transaction.
    sessionDB = session.getDatabase(kAdminDB);
    testSnapshotAggFailsWithCode(1, [{$currentOp: {}}], 50844);
    sessionDB = session.getDatabase(kDBName);

    // Helper for testing that aggregation stages which involve a local and foreign collection
    // ($lookup and $graphLookup) obey the expected readConcern "snapshot" isolation semantics.
    //
    // Inserts 'localDocsPre' into the 'local' collection and 'foreignDocsPre' into the 'foreign'
    // collection. Then runs the first batch of 'pipeline', before inserting 'localDocsPost' into
    // 'local' and 'foreignDocsPost' into 'foreign'. Iterates the remainder of the aggregation
    // cursor and verifies that the result set matches 'expectedResults'.
    function testLookupReadConcernSnapshotIsolation(
        {localDocsPre, foreignDocsPre, localDocsPost, foreignDocsPost, pipeline, expectedResults}) {
        sessionDB.runCommand({drop: "local", writeConcern: {w: "majority"}});
        sessionDB.runCommand({drop: "foreign", writeConcern: {w: "majority"}});
        let localColl = sessionDB.local;
        let foreignColl = sessionDB.foreign;
        assert.commandWorked(localColl.insert(localDocsPre, kWCMajority));
        assert.commandWorked(foreignColl.insert(foreignDocsPre, kWCMajority));
        let cmdRes = sessionDB.runCommand({
            aggregate: localColl.getName(),
            pipeline: pipeline,
            cursor: {batchSize: 0},
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(++txnNumber),
            startTransaction: true,
            autocommit: false
        });
        assert.commandWorked(cmdRes);
        assert.neq(0, cmdRes.cursor.id);
        assert.eq(0, cmdRes.cursor.firstBatch.length);

        assert.commandWorked(localColl.insert(localDocsPost, kWCMajority));
        assert.commandWorked(foreignColl.insert(foreignDocsPost, kWCMajority));
        let results =
            new DBCommandCursor(sessionDB, cmdRes, undefined, undefined, NumberLong(txnNumber))
                .toArray();
        assert.eq(results, expectedResults);
        assert.commandWorked(sessionDB.adminCommand(
            {commitTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}));
    }

    // Test that snapshot isolation works with $lookup using localField/foreignField syntax.
    testLookupReadConcernSnapshotIsolation({
        localDocsPre: [{_id: 0}, {_id: 1}, {_id: 2}],
        foreignDocsPre: [{_id: 1}],
        localDocsPost: [{_id: 3}],
        foreignDocsPost: [{_id: 2}, {_id: 3}],
        pipeline: [
            {$lookup: {from: "foreign", localField: "_id", foreignField: "_id", as: "as"}},
            {$sort: {_id: 1}}
        ],
        expectedResults: [{_id: 0, as: []}, {_id: 1, as: [{_id: 1}]}, {_id: 2, as: []}]
    });

    // Test that snapshot isolation works with $lookup into a nested pipeline.
    testLookupReadConcernSnapshotIsolation({
        localDocsPre: [{_id: 0}, {_id: 1}, {_id: 2}],
        foreignDocsPre: [{_id: 1}],
        localDocsPost: [{_id: 3}],
        foreignDocsPost: [{_id: 2}, {_id: 3}],
        pipeline: [
            {
              $lookup: {
                  from: "foreign",
                  as: "as",
                  let : {localId: "$_id"},
                  pipeline: [{$match: {$expr: {$eq: ["$_id", "$$localId"]}}}]
              }
            },
            {$sort: {_id: 1}}
        ],
        expectedResults: [{_id: 0, as: []}, {_id: 1, as: [{_id: 1}]}, {_id: 2, as: []}]
    });

    // Test that snapshot isolation works with $graphLookup.
    testLookupReadConcernSnapshotIsolation({
        localDocsPre: [{_id: 0}, {_id: 1}, {_id: 2}],
        foreignDocsPre: [{_id: 1, linkTo: 2}],
        localDocsPost: [{_id: 3}],
        foreignDocsPost: [{_id: 2, linkTo: 3}, {_id: 3}],
        pipeline: [
            {
              $graphLookup: {
                  from: "foreign",
                  as: "as",
                  startWith: "$_id",
                  connectFromField: "linkTo",
                  connectToField: "_id"
              }
            },
            {$sort: {_id: 1}}
        ],
        expectedResults:
            [{_id: 0, as: []}, {_id: 1, as: [{_id: 1, linkTo: 2}]}, {_id: 2, as: []}]
    });

    // Test that snapshot isolation works for $geoNear. Special care is taken to test snapshot
    // isolation across getMore for $geoNear as it is an initial document source.
    assert.commandWorked(sessionDB.runCommand({drop: kCollName, writeConcern: {w: "majority"}}));
    assert.commandWorked(sessionDB.runCommand({
        createIndexes: kCollName,
        indexes: [{key: {geo: "2dsphere"}, name: "geo_2dsphere"}],
        writeConcern: {w: "majority"}
    }));

    const coll = sessionDB.getCollection(kCollName);
    let bulk = coll.initializeUnorderedBulkOp();
    const numInitialGeoInsert = 4;
    for (let i = 0; i < numInitialGeoInsert; ++i) {
        bulk.insert({_id: i, geo: {type: "Point", coordinates: [0, 0]}});
    }
    assert.commandWorked(bulk.execute({w: "majority"}));

    let cmdRes = assert.commandWorked(sessionDB.runCommand({
        aggregate: kCollName,
        pipeline: [{
            $geoNear: {
                spherical: true,
                near: {type: "Point", coordinates: [0, 0]},
                distanceField: "distance"
            }
        }],
        txnNumber: NumberLong(++txnNumber),
        readConcern: {level: "snapshot"},
        autocommit: false,
        startTransaction: true,
        cursor: {batchSize: 0}
    }));
    assert(cmdRes.hasOwnProperty("cursor"));
    const cursorId = cmdRes.cursor.id;
    assert.neq(cursorId, 0);

    assert.commandWorked(
        coll.insert({_id: numInitialGeoInsert, geo: {type: "Point", coordinates: [0, 0]}},
                    {writeConcern: {w: "majority"}}));

    cmdRes = assert.commandWorked(sessionDB.runCommand({
        getMore: NumberLong(cursorId),
        collection: kCollName,
        autocommit: false,
        txnNumber: NumberLong(txnNumber)
    }));
    assert.commandWorked(sessionDB.adminCommand(
        {commitTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}));
    assert(cmdRes.hasOwnProperty("cursor"));
    assert(cmdRes.cursor.hasOwnProperty("nextBatch"));
    assert.eq(cmdRes.cursor.nextBatch.length, numInitialGeoInsert);

    // Test that snapshot reads are legal for $facet.
    assert.commandWorked(sessionDB.runCommand({drop: kCollName, writeConcern: {w: "majority"}}));
    assert.commandWorked(coll.insert(
        [
          {group1: 1, group2: 1, val: 1},
          {group1: 1, group2: 2, val: 2},
          {group1: 2, group2: 2, val: 8}
        ],
        kWCMajority));

    cmdRes = sessionDB.runCommand({
        aggregate: kCollName,
        pipeline: [
            {
              $facet: {
                  g1: [{$group: {_id: "$group1", sum: {$sum: "$val"}}}, {$sort: {_id: 1}}],
                  g2: [{$group: {_id: "$group2", sum: {$sum: "$val"}}}, {$sort: {_id: 1}}]
              }
            },
            {$unwind: "$g1"},
            {$unwind: "$g2"},
            {$sort: {"g1._id": 1, "g2._id": 1}}
        ],
        cursor: {},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(++txnNumber),
        startTransaction: true,
        autocommit: false
    });
    assert.commandWorked(sessionDB.adminCommand(
        {commitTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}));
    assert.commandWorked(cmdRes);
    assert.eq(0, cmdRes.cursor.id);
    assert.eq(cmdRes.cursor.firstBatch, [
        {g1: {_id: 1, sum: 3}, g2: {_id: 1, sum: 1}},
        {g1: {_id: 1, sum: 3}, g2: {_id: 2, sum: 10}},
        {g1: {_id: 2, sum: 8}, g2: {_id: 1, sum: 1}},
        {g1: {_id: 2, sum: 8}, g2: {_id: 2, sum: 10}}
    ]);

    rst.stopSet();
}());
