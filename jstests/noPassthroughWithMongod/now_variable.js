/**
 * Tests for the $$NOW and $$CLUSTER_TIME system variable.
 */
(function() {
    "use strict";

    const coll = db[jsTest.name()];
    const otherColl = db[coll.getName() + "_other"];
    otherColl.drop();
    coll.drop();
    db["viewWithNow"].drop();
    db["viewWithClusterTime"].drop();

    // Insert simple documents into the main test collection. Aggregation and view pipelines will
    // augment these docs with time-based fields.
    const numdocs = 1000;
    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numdocs; ++i) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());

    // Insert into another collection with pre-made fields for testing the find() command.
    bulk = otherColl.initializeUnorderedBulkOp();
    const timeFieldValue = new Date();
    for (let i = 0; i < numdocs; ++i) {
        bulk.insert({_id: i, timeField: timeFieldValue, clusterTimeField: new Timestamp(0, 1)});
    }
    assert.commandWorked(bulk.execute());

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

    function runTestsExpectFailure(query) {
        const results = query();
        // Expect to see "Builtin variable '$$CLUSTER_TIME' is not available" error.
        assert.commandFailedWithCode(results, 51144);
    }

    function baseCollectionNowFind() {
        return otherColl.find({$expr: {$lte: ["$timeField", "$$NOW"]}});
    }

    function baseCollectionClusterTimeFind() {
        return db.runCommand({
            find: otherColl.getName(),
            filter: {$expr: {$lt: ["$clusterTimeField", "$$CLUSTER_TIME"]}}
        });
    }

    function baseCollectionNowAgg() {
        return coll.aggregate([{$addFields: {timeField: "$$NOW"}}]);
    }

    function baseCollectionClusterTimeAgg() {
        return db.runCommand({
            aggregate: coll.getName(),
            pipeline: [{$addFields: {timeField: "$$CLUSTER_TIME"}}],
            cursor: {}
        });
    }

    function fromViewWithNow() {
        return viewWithNow.find();
    }

    function fromViewWithClusterTime() {
        return db.runCommand({find: viewWithClusterTime.getName()});
    }

    function withExprNow() {
        return viewWithNow.find({$expr: {$eq: ["$timeField", "$$NOW"]}});
    }

    function withExprClusterTime() {
        return db.runCommand({
            find: viewWithClusterTime.getName(),
            filter: {$expr: {$eq: ["$timeField", "$$CLUSTER_TIME"]}}
        });
    }

    // Test that $$NOW is usable in all contexts.
    runTests(baseCollectionNowFind);
    runTests(baseCollectionNowAgg);
    runTests(fromViewWithNow);
    runTests(withExprNow);

    // Test that $$NOW can be used in explain for both find and aggregate.
    assert.commandWorked(coll.explain().find({$expr: {$lte: ["$timeField", "$$NOW"]}}).finish());
    assert.commandWorked(
        viewWithNow.explain().find({$expr: {$eq: ["$timeField", "$$NOW"]}}).finish());
    assert.commandWorked(coll.explain().aggregate([{$addFields: {timeField: "$$NOW"}}]));

    // $$CLUSTER_TIME is not available on a standalone mongod.
    runTestsExpectFailure(baseCollectionClusterTimeFind);
    runTestsExpectFailure(baseCollectionClusterTimeAgg);
    runTestsExpectFailure(fromViewWithClusterTime);
    runTestsExpectFailure(withExprClusterTime);
}());
