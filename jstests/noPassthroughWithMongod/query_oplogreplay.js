// Test oplog queries that can be optimized with oplogReplay.
// @tags: [requires_replication, requires_capped]

(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");
    load("jstests/libs/storage_engine_utils.js");

    const t = db.getSiblingDB("local").oplog.jstests_query_oplogreplay;

    function dropOplogAndCreateNew(oplog, newCollectionSpec) {
        if (storageEngineIsWiredTigerOrInMemory()) {
            // We forbid dropping the oplog when using the WiredTiger or in-memory storage engines
            // and so we can't drop the oplog here. Because Evergreen reuses nodes for testing,
            // the oplog may already exist on the test node; in this case, trying to create the
            // oplog once again would fail.
            // To ensure we are working with a clean oplog (an oplog without entries), we resort
            // to truncating the oplog instead.
            if (!oplog.getDB().getCollectionNames().includes(oplog.getName())) {
                oplog.getDB().createCollection(oplog.getName(), newCollectionSpec);
            }
            oplog.runCommand('emptycapped');
            oplog.getDB().adminCommand({replSetResizeOplog: 1, size: 16 * 1024});
        } else {
            oplog.drop();
            assert.commandWorked(
                oplog.getDB().createCollection(oplog.getName(), newCollectionSpec));
        }
    }

    dropOplogAndCreateNew(t, {capped: true, size: 16 * 1024});

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

    // A $gt query on just the 'ts' field should return the next document after the timestamp.
    var cursor = t.find({ts: {$gt: makeTS(20)}});
    assert.eq(21, cursor.next()["_id"]);
    assert.eq(22, cursor.next()["_id"]);

    // A $gte query on the 'ts' field should include the timestamp.
    cursor = t.find({ts: {$gte: makeTS(20)}});
    assert.eq(20, cursor.next()["_id"]);
    assert.eq(21, cursor.next()["_id"]);

    // An $eq query on the 'ts' field should return the single record with the timestamp.
    cursor = t.find({ts: {$eq: makeTS(20)}});
    assert.eq(20, cursor.next()["_id"]);
    assert(!cursor.hasNext());

    // An AND with both a $gt and $lt query on the 'ts' field will correctly return results in
    // the proper bounds.
    cursor = t.find({$and: [{ts: {$lt: makeTS(5)}}, {ts: {$gt: makeTS(1)}}]});
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
    });
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
    });
    assert.eq(5, cursor.next()["_id"]);
    assert(!cursor.hasNext());

    // An $eq query stops scanning after passing the max timestamp.
    let res = t.find({ts: {$eq: makeTS(10)}}).explain("executionStats");
    assert.commandWorked(res);
    // We expect to be able to seek directly to the entry with a 'ts' of 10.
    assert.lte(res.executionStats.totalDocsExamined, 2, tojson(res));
    let collScanStage = getPlanStage(res.executionStats.executionStages, "COLLSCAN");
    assert.neq(null, collScanStage, "no collection scan found in explain output: " + tojson(res));
    assert.eq(makeTS(10), collScanStage.maxTs, tojson(res));

    // An AND with an $lt predicate stops scanning after passing the max timestamp.
    res = t.find({
               $and: [{ts: {$gte: makeTS(1)}}, {ts: {$lt: makeTS(10)}}]
           }).explain("executionStats");
    assert.commandWorked(res);
    assert.lte(res.executionStats.totalDocsExamined, 11, tojson(res));
    collScanStage = getPlanStage(res.executionStats.executionStages, "COLLSCAN");
    assert.neq(null, collScanStage, "no collection scan found in explain output: " + tojson(res));
    assert.eq(makeTS(10), collScanStage.maxTs, tojson(res));

    // An AND with an $lte predicate stops scanning after passing the max timestamp.
    res = t.find({
               $and: [{ts: {$gte: makeTS(1)}}, {ts: {$lte: makeTS(10)}}]
           }).explain("executionStats");
    assert.commandWorked(res);
    assert.lte(res.executionStats.totalDocsExamined, 12, tojson(res));
    collScanStage = getPlanStage(res.executionStats.executionStages, "COLLSCAN");
    assert.neq(null, collScanStage, "no collection scan found in explain output: " + tojson(res));
    assert.eq(makeTS(10), collScanStage.maxTs, tojson(res));

    // The max timestamp is respected even when the min timestamp is smaller than the lowest
    // timestamp in the collection.
    res = t.find({
               $and: [{ts: {$gte: makeTS(0)}}, {ts: {$lte: makeTS(10)}}]
           }).explain("executionStats");
    assert.commandWorked(res);
    assert.lte(res.executionStats.totalDocsExamined, 12, tojson(res));
    collScanStage = getPlanStage(res.executionStats.executionStages, "COLLSCAN");
    assert.neq(null, collScanStage, "no collection scan found in explain output: " + tojson(res));
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
           }).explain("executionStats");
    assert.commandWorked(res);
    // We expect to be able to seek directly to the entry with a 'ts' of 5.
    assert.lte(res.executionStats.totalDocsExamined, 2, tojson(res));
    collScanStage = getPlanStage(res.executionStats.executionStages, "COLLSCAN");
    assert.neq(null, collScanStage, "no collection scan found in explain output: " + tojson(res));
    assert.eq(makeTS(5), collScanStage.maxTs, tojson(res));
    assert.eq(makeTS(5), collScanStage.minTs, tojson(res));

    // An $eq query for a non-existent timestamp scans a single oplog document.
    res = t.find({ts: {$eq: makeTS(200)}}).explain("executionStats");
    assert.commandWorked(res);
    // We expect to be able to seek directly to the end of the oplog.
    assert.lte(res.executionStats.totalDocsExamined, 1, tojson(res));
    collScanStage = getPlanStage(res.executionStats.executionStages, "COLLSCAN");
    assert.neq(null, collScanStage, "no collection scan found in explain output: " + tojson(res));
    assert.eq(makeTS(200), collScanStage.maxTs, tojson(res));

    // When the filter matches the last document within the timestamp range, the collection scan
    // examines at most one more document.
    res = t.find({
               $and: [{ts: {$gte: makeTS(4)}}, {ts: {$lte: makeTS(8)}}, {_id: 8}]
           }).explain("executionStats");
    assert.commandWorked(res);
    // We expect to be able to seek directly to the start of the 'ts' range.
    assert.lte(res.executionStats.totalDocsExamined, 6, tojson(res));
    collScanStage = getPlanStage(res.executionStats.executionStages, "COLLSCAN");
    assert.neq(null, collScanStage, "no collection scan found in explain output: " + tojson(res));
    assert.eq(makeTS(8), collScanStage.maxTs, tojson(res));

    // A filter with only an upper bound predicate on 'ts' stops scanning after
    // passing the max timestamp.
    res = t.find({ts: {$lt: makeTS(4)}}).explain("executionStats");
    assert.commandWorked(res);
    assert.lte(res.executionStats.totalDocsExamined, 4, tojson(res));
    collScanStage = getPlanStage(res.executionStats.executionStages, "COLLSCAN");
    assert.neq(null, collScanStage, "no collection scan found in explain output: " + tojson(res));
    assert.eq(makeTS(4), collScanStage.maxTs, tojson(res));

    // Oplog replay optimization should work with projection.
    res = t.find({ts: {$lte: makeTS(4)}}).projection({'_id': 0});
    while (res.hasNext()) {
        const next = res.next();
        assert(!next.hasOwnProperty('_id'));
        assert(next.hasOwnProperty('ts'));
    }
    res = res.explain("executionStats");
    assert.commandWorked(res);
    assert.lte(res.executionStats.totalDocsExamined, 5);

    res = t.find({ts: {$gte: makeTS(90)}}).projection({'_id': 0});
    while (res.hasNext()) {
        const next = res.next();
        assert(!next.hasOwnProperty('_id'));
        assert(next.hasOwnProperty('ts'));
    }
    res = res.explain("executionStats");
    assert.commandWorked(res);
    assert.lte(res.executionStats.totalDocsExamined, 11);

    // Oplog replay optimization should work with limit.
    res = t.find({$and: [{ts: {$gte: makeTS(4)}}, {ts: {$lte: makeTS(8)}}]})
              .limit(2)
              .explain("executionStats");
    assert.commandWorked(res);
    assert.eq(2, res.executionStats.totalDocsExamined);
    collScanStage = getPlanStage(res.executionStats.executionStages, "COLLSCAN");
    assert.eq(2, collScanStage.nReturned);

    // A query over both 'ts' and '_id' should only pay attention to the 'ts' field for finding
    // the oplog start (SERVER-13566).
    cursor = t.find({ts: {$gte: makeTS(20)}, _id: 25});
    assert.eq(25, cursor.next()["_id"]);
    assert(!cursor.hasNext());

    // 'oplogreplay' flag is allowed but ignored on the oplog collection.
    assert.commandWorked(t.runCommand({find: t.getName(), oplogReplay: true}));

    // 'oplogreplay' flag is allowed but ignored on capped collections.
    const cappedColl = db.cappedColl_jstests_query_oplogreplay;
    cappedColl.drop();
    assert.commandWorked(
        db.createCollection(cappedColl.getName(), {capped: true, size: 16 * 1024}));
    for (let i = 1; i <= 100; i++) {
        assert.commandWorked(cappedColl.insert({_id: i, ts: makeTS(i)}));
    }
    res = cappedColl.runCommand({
        explain:
            {find: cappedColl.getName(), filter: {ts: {$eq: makeTS(200)}}, oplogReplay: true}
    });
    assert.commandWorked(res);
    assert.eq(res.executionStats.totalDocsExamined, 100);

    // Ensure oplog replay hack does not work for backward scans.
    res = t.find({ts: {$lt: makeTS(4)}}).sort({$natural: -1}).explain("executionStats");
    assert.commandWorked(res);
    assert.eq(res.executionStats.totalDocsExamined, 100, tojson(res));
    collScanStage = getPlanStage(res.executionStats.executionStages, "COLLSCAN");
    assert.neq(null, collScanStage, "no collection scan found in explain output: " + tojson(res));

    // We expect correct results when no collation specified and collection has a default collation.
    const t_collation = db.getSiblingDB("local").oplog.jstests_query_oplogreplay_collation;
    dropOplogAndCreateNew(
        t_collation, {collation: {locale: "en_US", strength: 2}, capped: true, size: 16 * 1024});

    assert.writeOK(t_collation.insert({str: "FOO", ts: Timestamp(1000, 0)}));
    assert.writeOK(t_collation.insert({str: "FOO", ts: Timestamp(1000, 1)}));
    assert.writeOK(t_collation.insert({str: "FOO", ts: Timestamp(1000, 2)}));
    assert.eq(2, t_collation.find({str: "foo", ts: {$gte: Timestamp(1000, 1)}}).itcount());

    // We expect correct results when "simple" collation specified and collection has a default
    // collation.
    assert.writeOK(t_collation.insert({str: "FOO", ts: Timestamp(1000, 0)}));
    assert.writeOK(t_collation.insert({str: "FOO", ts: Timestamp(1000, 1)}));
    assert.writeOK(t_collation.insert({str: "FOO", ts: Timestamp(1000, 2)}));
    assert.eq(0,
              t_collation.find({str: "foo", ts: {$gte: Timestamp(1000, 1)}})
                  .collation({locale: "simple"})
                  .itcount());
}());
