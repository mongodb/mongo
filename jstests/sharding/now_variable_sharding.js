/**
 * Tests for the $$NOW and $$CLUSTER_TIME system variable on a sharded cluster.
 */
// @tags: [requires_find_command]
(function() {
    "use strict";

    var st = new ShardingTest({mongos: 1, shards: 2});

    // Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
    st.adminCommand({enableSharding: "test"});
    st.ensurePrimaryShard("test", st.rs0.getURL());

    var db = st.s.getDB("test");

    const numdocs = 1000;

    const coll = db[jsTest.name()];

    coll.createIndex({_id: 1}, {unique: true});

    st.adminCommand({shardcollection: coll.getFullName(), key: {_id: 1}});
    st.adminCommand({split: coll.getFullName(), middle: {_id: numdocs / 2}});

    st.adminCommand({
        moveChunk: coll.getFullName(),
        find: {_id: 0},
        to: st.shard1.shardName,
        _waitForDelete: true
    });
    st.adminCommand({
        moveChunk: coll.getFullName(),
        find: {_id: numdocs / 2},
        to: st.shard0.shardName,
        _waitForDelete: true
    });

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

    st.stop();
}());
