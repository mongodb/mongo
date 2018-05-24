// @tags: [requires_replication]

// Test the correct timestamping of insert, update, and delete writes along with their accompanying
// index updates.
//

(function() {
    "use strict";

    const dbName = "test";
    const collName = "coll";

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const testDB = rst.getPrimary().getDB(dbName);
    const coll = testDB.getCollection(collName);

    if (!testDB.serverStatus().storageEngine.supportsSnapshotReadConcern) {
        rst.stopSet();
        return;
    }

    // Turn off timestamp reaping.
    assert.commandWorked(testDB.adminCommand({
        configureFailPoint: "WTPreserveSnapshotHistoryIndefinitely",
        mode: "alwaysOn",
    }));

    const session = testDB.getMongo().startSession({causalConsistency: false});
    const sessionDb = session.getDatabase(dbName);
    const response = assert.commandWorked(testDB.createCollection("coll"));
    const startTime = response.operationTime;
    let txnNumber = 0;

    function check(atClusterTime, expected) {
        // Check both a collection scan and scanning the _id index.
        [{$natural: 1}, {_id: 1}].forEach(sort => {
            let response = assert.commandWorked(sessionDb.runCommand({
                find: collName,
                sort: sort,
                readConcern: {level: "snapshot", atClusterTime: atClusterTime},
                txnNumber: NumberLong(txnNumber++),
                singleBatch: true
            }));
            assert.eq(expected, response.cursor.firstBatch);
        });
    }

    // insert

    let request = {insert: coll.getName(), documents: [{_id: 1}, {_id: 2}], ordered: false};
    assert.commandWorked(coll.runCommand(request));

    const oplog = rst.getPrimary().getDB("local").getCollection("oplog.rs");
    let ts1 = oplog.findOne({o: {_id: 1}}).ts;
    let ts2 = oplog.findOne({o: {_id: 2}}).ts;

    check(startTime, []);
    check(ts1, [{_id: 1}]);
    check(ts2, [{_id: 1}, {_id: 2}]);

    // upsert

    request = {
        update: coll.getName(),
        updates: [
            {q: {_id: 3, a: 1}, u: {$set: {a: 2}}, upsert: true},
            {q: {_id: 4, a: 1}, u: {$set: {a: 3}}, upsert: true}
        ],
        ordered: true
    };
    assert.commandWorked(coll.runCommand(request));

    ts1 = oplog.findOne({o: {_id: 3, a: 2}}).ts;
    ts2 = oplog.findOne({o: {_id: 4, a: 3}}).ts;

    check(ts1, [{_id: 1}, {_id: 2}, {_id: 3, a: 2}]);
    check(ts2, [{_id: 1}, {_id: 2}, {_id: 3, a: 2}, {_id: 4, a: 3}]);

    // update

    request = {
        update: coll.getName(),
        updates: [{q: {_id: 3, a: 2}, u: {$set: {a: 4}}}, {q: {_id: 4, a: 3}, u: {$set: {a: 5}}}],
        ordered: true
    };
    assert.commandWorked(coll.runCommand(request));

    ts1 = oplog.findOne({op: 'u', o2: {_id: 3}}).ts;
    ts2 = oplog.findOne({op: 'u', o2: {_id: 4}}).ts;

    check(ts1, [{_id: 1}, {_id: 2}, {_id: 3, a: 4}, {_id: 4, a: 3}]);
    check(ts2, [{_id: 1}, {_id: 2}, {_id: 3, a: 4}, {_id: 4, a: 5}]);

    // delete

    request = {delete: coll.getName(), deletes: [{q: {}, limit: 0}], ordered: false};

    assert.commandWorked(coll.runCommand(request));

    ts1 = oplog.findOne({op: 'd', o: {_id: 1}}).ts;
    ts2 = oplog.findOne({op: 'd', o: {_id: 2}}).ts;
    let ts3 = oplog.findOne({op: 'd', o: {_id: 3}}).ts;
    let ts4 = oplog.findOne({op: 'd', o: {_id: 4}}).ts;

    check(ts1, [{_id: 2}, {_id: 3, a: 4}, {_id: 4, a: 5}]);
    check(ts2, [{_id: 3, a: 4}, {_id: 4, a: 5}]);
    check(ts3, [{_id: 4, a: 5}]);
    check(ts4, []);

    session.endSession();
    rst.stopSet();

}());
