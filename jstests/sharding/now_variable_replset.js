/**
 * Tests for the $$NOW and $$CLUSTER_TIME system variable on a replica set.
 */
// @tags: [requires_find_command]
(function() {
    "use strict";

    var replTest = new ReplSetTest({name: "now_and_cluster_time", nodes: 1});
    replTest.startSet();
    replTest.initiate();

    var db = replTest.getPrimary().getDB("test");

    const coll = db[jsTest.name()];
    coll.drop();
    db["viewWithNow"].drop();
    db["viewWithClusterTime"].drop();

    const numdocs = 1000;
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numdocs; ++i) {
        bulk.insert({_id: i});
    }
    assert.writeOK(bulk.execute());

    assert.commandWorked(
        db.createView("viewWithNow", coll.getName(), [{$addFields: {timeField: "$$NOW"}}]));
    const viewWithNow = db["viewWithNow"];

    assert.commandWorked(db.createView(
        "viewWithClusterTime", coll.getName(), [{$addFields: {timeField: "$$CLUSTER_TIME"}}]));
    const viewWithClusterTime = db["viewWithClusterTime"];

    function runTests(query) {
        const results = query().toArray();
        assert.eq(results.length, numdocs);

        // Make sure the values are the same for all documents
        for (let i = 0; i < numdocs; ++i) {
            assert.eq(results[0].timeField, results[i].timeField);
        }

        // Sleep for a while and then rerun.
        sleep(3000);

        const resultsLater = query().toArray();
        assert.eq(resultsLater.length, numdocs);

        // Later results should be later in time.
        assert.lte(results[0].timeField, resultsLater[0].timeField);
    }

    function baseCollectionNow() {
        return coll.aggregate([{$addFields: {timeField: "$$NOW"}}]);
    }

    function baseCollectionClusterTime() {
        return coll.aggregate([{$addFields: {timeField: "$$CLUSTER_TIME"}}]);
    }

    function fromViewWithNow() {
        return viewWithNow.find();
    }

    function fromViewWithClusterTime() {
        return viewWithClusterTime.find();
    }

    function withExprNow() {
        return viewWithNow.find({$expr: {$eq: ["$timeField", "$$NOW"]}});
    }

    function withExprClusterTime() {
        return viewWithClusterTime.find({$expr: {$eq: ["$timeField", "$$CLUSTER_TIME"]}});
    }

    // $$NOW
    runTests(baseCollectionNow);
    runTests(fromViewWithNow);
    runTests(withExprNow);

    // $$CLUSTER_TIME
    runTests(baseCollectionClusterTime);
    runTests(fromViewWithClusterTime);
    runTests(withExprClusterTime);

    replTest.stopSet();
}());
