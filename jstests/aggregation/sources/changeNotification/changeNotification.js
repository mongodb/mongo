// Basic $changeNotification tests.
// @tags: [do_not_wrap_aggregations_in_facets]

(function() {
    "use strict";

    // Helper for testing that pipeline returns correct set of results.
    function testPipeline(pipeline, expectedResult, collection) {
        // Strip the oplog fields we aren't testing.
        pipeline.push({$project: {ts: 0, t: 0, h: 0, v: 0, wt: 0}});
        assert.docEq(collection.aggregate(pipeline).toArray(), expectedResult);
    }

    var replTest = new ReplSetTest({name: 'changeNotificationTest', nodes: 1});
    var nodes = replTest.startSet();
    replTest.initiate();
    replTest.awaitReplication();

    db = replTest.getPrimary().getDB('test');

    jsTestLog("Testing single insert");
    assert.writeOK(db.t1.insert({_id: 0, a: 1}));
    testPipeline([{$changeNotification: {}}], [{op: "i", ns: "test.t1", o: {_id: 0, a: 1}}], db.t1);

    jsTestLog("Testing second insert");
    assert.writeOK(db.t1.insert({_id: 1, a: 2}));
    testPipeline([{$changeNotification: {}}], [{op: "i", ns: "test.t1", o: {_id: 1, a: 2}}], db.t1);

    jsTestLog("Testing update");
    assert.writeOK(db.t1.update({_id: 0}, {a: 3}));
    testPipeline([{$changeNotification: {}}],
                 [{op: "u", ns: "test.t1", o: {_id: 0, a: 3}, o2: {_id: 0}}],
                 db.t1);

    jsTestLog("Testing update of another field");
    assert.writeOK(db.t1.update({_id: 0}, {b: 3}));
    testPipeline([{$changeNotification: {}}],
                 [{op: "u", ns: "test.t1", o: {_id: 0, b: 3}, o2: {_id: 0}}],
                 db.t1);

    jsTestLog("Testing upsert");
    assert.writeOK(db.t1.update({_id: 2}, {a: 4}, {upsert: true}));
    testPipeline([{$changeNotification: {}}], [{op: "i", ns: "test.t1", o: {_id: 2, a: 4}}], db.t1);

    jsTestLog("Testing partial update with $inc");
    assert.writeOK(db.t1.insert({_id: 3, a: 5, b: 1}));
    assert.writeOK(db.t1.update({_id: 3}, {$inc: {b: 2}}));
    testPipeline([{$changeNotification: {}}],
                 [{op: "u", ns: "test.t1", o: {$set: {b: 3}}, o2: {_id: 3}}],
                 db.t1);

    jsTestLog("Testing delete");
    assert.writeOK(db.t1.remove({_id: 1}));
    testPipeline([{$changeNotification: {}}], [{op: "d", ns: "test.t1", o: {_id: 1}}], db.t1);

    jsTestLog("Testing intervening write on another collection");
    assert.writeOK(db.t2.insert({_id: 100, c: 1}));
    testPipeline([{$changeNotification: {}}], [{op: "d", ns: "test.t1", o: {_id: 1}}], db.t1);
    testPipeline(
        [{$changeNotification: {}}], [{op: "i", ns: "test.t2", o: {_id: 100, c: 1}}], db.t2);

    jsTestLog("Testing rename");
    assert.writeOK(db.t2.renameCollection("t3"));
    testPipeline(
        [{$changeNotification: {}}],
        [{
           op: "c",
           ns: "test.$cmd",
           o: {renameCollection: "test.t2", to: "test.t3", dropTarget: false, stayTemp: false}
        }],
        db.t2);

    jsTestLog("Testing insert that looks like rename");
    assert.writeOK(db.t3.insert({_id: 101, renameCollection: "test.dne1", to: "test.dne2"}));
    testPipeline([{$changeNotification: {}}], [], db.dne1);
    testPipeline([{$changeNotification: {}}], [], db.dne2);
    replTest.stopSet();
}());
