// Basic $changeNotification tests.
// @tags: [do_not_wrap_aggregations_in_facets]

(function() {
    "use strict";

    // Helper for testing that pipeline returns correct set of results.
    function testPipeline(pipeline, expectedResult, collection) {
        // Strip the oplog fields we aren't testing.
        pipeline.push({$limit: 1});
        pipeline.push({$project: {"_id.ts": 0}});
        assert.docEq(collection.aggregate(pipeline).toArray(), expectedResult);
    }

    var replTest = new ReplSetTest({name: 'changeNotificationTest', nodes: 1});
    var nodes = replTest.startSet();
    replTest.initiate();
    replTest.awaitReplication();

    db = replTest.getPrimary().getDB('test');

    jsTestLog("Testing single insert");
    assert.writeOK(db.t1.insert({_id: 0, a: 1}));
    let expected = {
        "_id": {
            "_id": 0,
            "ns": "test.t1",
        },
        "documentKey": {"_id": 0},
        "newDocument": {"_id": 0, "a": 1},
        "ns": {"coll": "t1", "db": "test"},
        "operationType": "insert"
    };
    testPipeline([{$changeNotification: {}}], [expected], db.t1);

    jsTestLog("Testing second insert");
    assert.writeOK(db.t1.insert({_id: 1, a: 2}));
    expected = {
        "_id": {
            "_id": 1,
            "ns": "test.t1",
        },
        "documentKey": {"_id": 1},
        "newDocument": {"_id": 1, "a": 2},
        "ns": {"coll": "t1", "db": "test"},
        "operationType": "insert"
    };
    testPipeline([{$changeNotification: {}}], [expected], db.t1);

    jsTestLog("Testing update");
    assert.writeOK(db.t1.update({_id: 0}, {a: 3}));
    expected = {
        "_id": {"_id": 0, "ns": "test.t1"},
        "documentKey": {"_id": 0},
        "newDocument": {"_id": 0, "a": 3},
        "ns": {"coll": "t1", "db": "test"},
        "operationType": "replace"
    };
    testPipeline([{$changeNotification: {}}], [expected], db.t1);

    jsTestLog("Testing update of another field");
    assert.writeOK(db.t1.update({_id: 0}, {b: 3}));
    expected = {
        "_id": {"_id": 0, "ns": "test.t1"},
        "documentKey": {"_id": 0},
        "newDocument": {"_id": 0, "b": 3},
        "ns": {"coll": "t1", "db": "test"},
        "operationType": "replace"
    };
    testPipeline([{$changeNotification: {}}], [expected], db.t1);

    jsTestLog("Testing upsert");
    assert.writeOK(db.t1.update({_id: 2}, {a: 4}, {upsert: true}));
    expected = {
        "_id": {
            "_id": 2,
            "ns": "test.t1",
        },
        "documentKey": {"_id": 2},
        "newDocument": {"_id": 2, "a": 4},
        "ns": {"coll": "t1", "db": "test"},
        "operationType": "insert"
    };
    testPipeline([{$changeNotification: {}}], [expected], db.t1);

    jsTestLog("Testing partial update with $inc");
    assert.writeOK(db.t1.insert({_id: 3, a: 5, b: 1}));
    assert.writeOK(db.t1.update({_id: 3}, {$inc: {b: 2}}));
    expected = {
        "_id": {"_id": 3, "ns": "test.t1"},
        "documentKey": {"_id": 3},
        "ns": {"coll": "t1", "db": "test"},
        "operationType": "update",
        "updateDescription": {"removedFields": [], "updatedFields": {"b": 3}}
    };
    testPipeline([{$changeNotification: {}}], [expected], db.t1);

    jsTestLog("Testing delete");
    assert.writeOK(db.t1.remove({_id: 1}));
    expected = {
        "_id": {"_id": 1, "ns": "test.t1"},
        "documentKey": {"_id": 1},
        "ns": {"coll": "t1", "db": "test"},
        "operationType": "delete"
    };
    testPipeline([{$changeNotification: {}}], [expected], db.t1);

    jsTestLog("Testing intervening write on another collection");
    assert.writeOK(db.t2.insert({_id: 100, c: 1}));
    testPipeline([{$changeNotification: {}}], [expected], db.t1);
    expected = {
        "_id": {
            "_id": 100,
            "ns": "test.t2",
        },
        "documentKey": {"_id": 100},
        "newDocument": {"_id": 100, "c": 1},
        "ns": {"coll": "t2", "db": "test"},
        "operationType": "insert"
    };
    testPipeline([{$changeNotification: {}}], [expected], db.t2);

    jsTestLog("Testing rename");
    assert.writeOK(db.t2.renameCollection("t3"));
    expected = {"_id": {"ns": "test.$cmd"}, "operationType": "invalidate"};
    testPipeline([{$changeNotification: {}}], [expected], db.t2);

    jsTestLog("Testing insert that looks like rename");
    assert.writeOK(db.t3.insert({_id: 101, renameCollection: "test.dne1", to: "test.dne2"}));
    testPipeline([{$changeNotification: {}}], [], db.dne1);
    testPipeline([{$changeNotification: {}}], [], db.dne2);
    replTest.stopSet();
}());
