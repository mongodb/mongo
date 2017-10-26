// Test queries that set the OplogReplay flag.

(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");

    function test(t) {
        t.drop();
        assert.commandWorked(
            t.getDB().createCollection(t.getName(), {capped: true, size: 16 * 1024}));

        /**
         * Helper function for making timestamps with the property that if i < j, then makeTS(i) <
         * makeTS(j).
         */
        function makeTS(i) {
            return Timestamp(1000, i);
        }

        for (let i = 1; i <= 100; i++) {
            assert.writeOK(t.insert({_id: i, ts: makeTS(i)}));
        }

        // Missing 'ts' field.
        assert.throws(function() {
            t.find().addOption(DBQuery.Option.oplogReplay).next();
        });
        assert.throws(function() {
            t.find({_id: 3}).addOption(DBQuery.Option.oplogReplay).next();
        });

        // 'ts' field is not top-level.
        assert.throws(function() {
            t.find({$or: [{ts: {$gt: makeTS(3)}}, {foo: 3}]})
                .addOption(DBQuery.Option.oplogReplay)
                .next();
        });
        assert.throws(function() {
            t.find({$nor: [{ts: {$gt: makeTS(4)}}, {foo: 4}]})
                .addOption(DBQuery.Option.oplogReplay)
                .next();
        });

        // There is no $eq, $gt or $gte predicate on 'ts'.
        assert.throws(function() {
            t.find({ts: {$lt: makeTS(4)}}).addOption(DBQuery.Option.oplogReplay).next();
        });
        assert.throws(function() {
            t.find({ts: {$lt: makeTS(4)}, _id: 3}).addOption(DBQuery.Option.oplogReplay).next();
        });

        // A $gt query on just the 'ts' field should return the next document after the timestamp.
        var cursor = t.find({ts: {$gt: makeTS(20)}}).addOption(DBQuery.Option.oplogReplay);
        assert.eq(21, cursor.next()["_id"]);
        assert.eq(22, cursor.next()["_id"]);

        // A $gte query on the 'ts' field should include the timestamp.
        cursor = t.find({ts: {$gte: makeTS(20)}}).addOption(DBQuery.Option.oplogReplay);
        assert.eq(20, cursor.next()["_id"]);
        assert.eq(21, cursor.next()["_id"]);

        // An $eq query on the 'ts' field should return the single record with the timestamp.
        cursor = t.find({ts: {$eq: makeTS(20)}}).addOption(DBQuery.Option.oplogReplay);
        assert.eq(20, cursor.next()["_id"]);
        assert(!cursor.hasNext());

        // An AND with both a $gt and $lt query on the 'ts' field will correctly return results in
        // the proper bounds.
        cursor = t.find({
                      $and: [{ts: {$lt: makeTS(5)}}, {ts: {$gt: makeTS(1)}}]
                  }).addOption(DBQuery.Option.oplogReplay);
        assert.eq(2, cursor.next()["_id"]);
        assert.eq(3, cursor.next()["_id"]);
        assert.eq(4, cursor.next()["_id"]);
        assert(!cursor.hasNext());

        // An AND with multiple predicates on the 'ts' field correctly returns results on the
        // tightest range.
        cursor = t.find({
                      $and: [
                          {ts: {$gte: makeTS(2)}},
                          {ts: {$gt: makeTS(3)}},
                          {ts: {$lte: makeTS(7)}},
                          {ts: {$lt: makeTS(7)}}
                      ]
                  }).addOption(DBQuery.Option.oplogReplay);
        assert.eq(4, cursor.next()["_id"]);
        assert.eq(5, cursor.next()["_id"]);
        assert.eq(6, cursor.next()["_id"]);
        assert(!cursor.hasNext());

        // An AND with an $eq predicate in conjunction with other bounds correctly returns one
        // result.
        cursor = t.find({
                      $and: [
                          {ts: {$gte: makeTS(1)}},
                          {ts: {$gt: makeTS(2)}},
                          {ts: {$eq: makeTS(5)}},
                          {ts: {$lte: makeTS(8)}},
                          {ts: {$lt: makeTS(8)}}
                      ]
                  }).addOption(DBQuery.Option.oplogReplay);
        assert.eq(5, cursor.next()["_id"]);
        assert(!cursor.hasNext());

        // An $eq query stops scanning after passing the max timestamp.
        let res = t.find({ts: {$eq: makeTS(10)}})
                      .addOption(DBQuery.Option.oplogReplay)
                      .explain("executionStats");
        assert.commandWorked(res);
        assert.lte(res.executionStats.totalDocsExamined, 2, tojson(res));
        let collScanStage = getPlanStage(res.executionStats.executionStages, "COLLSCAN");
        assert.neq(
            null, collScanStage, "no collection scan found in explain output: " + tojson(res));
        assert.eq(makeTS(10), collScanStage.maxTs, tojson(res));

        // An AND with an $lt predicate stops scanning after passing the max timestamp.
        res = t.find({$and: [{ts: {$gte: makeTS(1)}}, {ts: {$lt: makeTS(10)}}]})
                  .addOption(DBQuery.Option.oplogReplay)
                  .explain("executionStats");
        assert.commandWorked(res);
        assert.lte(res.executionStats.totalDocsExamined, 11, tojson(res));
        collScanStage = getPlanStage(res.executionStats.executionStages, "COLLSCAN");
        assert.neq(
            null, collScanStage, "no collection scan found in explain output: " + tojson(res));
        assert.eq(makeTS(10), collScanStage.maxTs, tojson(res));

        // An AND with an $lte predicate stops scanning after passing the max timestamp.
        res = t.find({$and: [{ts: {$gte: makeTS(1)}}, {ts: {$lte: makeTS(10)}}]})
                  .addOption(DBQuery.Option.oplogReplay)
                  .explain("executionStats");
        assert.commandWorked(res);
        assert.lte(res.executionStats.totalDocsExamined, 12, tojson(res));
        collScanStage = getPlanStage(res.executionStats.executionStages, "COLLSCAN");
        assert.neq(
            null, collScanStage, "no collection scan found in explain output: " + tojson(res));
        assert.eq(makeTS(10), collScanStage.maxTs, tojson(res));

        // The max timestamp is respected even when the min timestamp is smaller than the lowest
        // timestamp in the collection.
        res = t.find({$and: [{ts: {$gte: makeTS(0)}}, {ts: {$lte: makeTS(10)}}]})
                  .addOption(DBQuery.Option.oplogReplay)
                  .explain("executionStats");
        assert.commandWorked(res);
        assert.lte(res.executionStats.totalDocsExamined, 12, tojson(res));
        collScanStage = getPlanStage(res.executionStats.executionStages, "COLLSCAN");
        assert.neq(
            null, collScanStage, "no collection scan found in explain output: " + tojson(res));
        assert.eq(makeTS(10), collScanStage.maxTs, tojson(res));

        // An AND with redundant $eq/$lt/$lte predicates stops scanning after passing the max
        // timestamp.
        res = t.find({
                   $and: [
                       {ts: {$gte: makeTS(0)}},
                       {ts: {$lte: makeTS(10)}},
                       {ts: {$eq: makeTS(5)}},
                       {ts: {$lt: makeTS(20)}}
                   ]
               })
                  .addOption(DBQuery.Option.oplogReplay)
                  .explain("executionStats");
        assert.commandWorked(res);
        assert.lte(res.executionStats.totalDocsExamined, 2, tojson(res));
        collScanStage = getPlanStage(res.executionStats.executionStages, "COLLSCAN");
        assert.neq(
            null, collScanStage, "no collection scan found in explain output: " + tojson(res));
        assert.eq(makeTS(5), collScanStage.maxTs, tojson(res));

        // An $eq query for a non-existent timestamp scans a single document.
        res = t.find({ts: {$eq: makeTS(200)}})
                  .addOption(DBQuery.Option.oplogReplay)
                  .explain("executionStats");
        assert.commandWorked(res);
        assert.lte(res.executionStats.totalDocsExamined, 1, tojson(res));
        collScanStage = getPlanStage(res.executionStats.executionStages, "COLLSCAN");
        assert.neq(
            null, collScanStage, "no collection scan found in explain output: " + tojson(res));
        assert.eq(makeTS(200), collScanStage.maxTs, tojson(res));

        // When the filter matches the last document within the timestamp range, the collection scan
        // examines at most one more document.
        res = t.find({$and: [{ts: {$gte: makeTS(4)}}, {ts: {$lte: makeTS(8)}}, {_id: 8}]})
                  .addOption(DBQuery.Option.oplogReplay)
                  .explain("executionStats");
        assert.commandWorked(res);
        assert.lte(res.executionStats.totalDocsExamined, 6, tojson(res));
        collScanStage = getPlanStage(res.executionStats.executionStages, "COLLSCAN");
        assert.neq(
            null, collScanStage, "no collection scan found in explain output: " + tojson(res));
        assert.eq(makeTS(8), collScanStage.maxTs, tojson(res));

        // A query over both 'ts' and '_id' should only pay attention to the 'ts' field for finding
        // the oplog start (SERVER-13566).
        cursor = t.find({ts: {$gte: makeTS(20)}, _id: 25}).addOption(DBQuery.Option.oplogReplay);
        assert.eq(25, cursor.next()["_id"]);
        assert(!cursor.hasNext());
    }

    // Test that oplog replay on a non-oplog collection succeeds.
    test(db.jstests_query_oplogreplay);

    // Test that oplog replay on the actual oplog succeeds.
    test(db.getSiblingDB("local").oplog.jstests_query_oplogreplay);

    // Test that oplog replay on a non-capped collection fails.
    const coll = db.jstests_query_oplogreplay;
    coll.drop();
    assert.commandWorked(coll.getDB().createCollection(coll.getName()));
    assert.throws(function() {
        coll.find({ts: {$gt: "abcd"}}).addOption(DBQuery.Option.oplogReplay).next();
    });
}());
