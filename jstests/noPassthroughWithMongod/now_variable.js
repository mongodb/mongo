/**
 * Tests for the $$NOW and $$CLUSTER_TIME system variable.
 */
(function() {
    "use strict";

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

    function runTestsExpectFailure(query) {
        const results = query();
        // Expect to see "Buildin variable '$$CLUSTER_TIME' is not available" error.
        assert.commandFailedWithCode(results, 51144);
    }

    function baseCollectionNow() {
        return coll.aggregate([{$addFields: {timeField: "$$NOW"}}]);
    }

    function baseCollectionClusterTime() {
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

    runTests(baseCollectionNow);
    runTests(fromViewWithNow);
    runTests(withExprNow);

    // $$CLUSTER_TIME is not available on a standalone mongod.
    runTestsExpectFailure(baseCollectionClusterTime);
    runTestsExpectFailure(fromViewWithClusterTime);
    runTestsExpectFailure(withExprClusterTime);
}());
