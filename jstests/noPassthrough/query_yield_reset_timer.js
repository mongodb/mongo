// Tests the reset logic for the periodic query yield timer.  Regression test for SERVER-21341.
(function() {
    'use strict';
    var dbpath = MongoRunner.dataPath + jsTest.name();
    resetDbpath(dbpath);
    var mongod = MongoRunner.runMongod({dbpath: dbpath});
    var coll = mongod.getDB("test").getCollection(jsTest.name());

    // Configure the server so that queries are expected to yield after every 10 work cycles, or
    // after every 500 milliseconds (whichever comes first). In addition, enable a failpoint that
    // introduces a sleep delay of 1 second during each yield.
    assert.commandWorked(
        coll.getDB().adminCommand({setParameter: 1, internalQueryExecYieldIterations: 10}));
    assert.commandWorked(
        coll.getDB().adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: 500}));
    assert.commandWorked(coll.getDB().adminCommand({
        configureFailPoint: "setYieldAllLocksWait",
        namespace: coll.getFullName(),
        mode: "alwaysOn",
        data: {waitForMillis: 1000}
    }));

    // Insert 40 documents in the collection, perform a collection scan, and verify that it yields
    // about 4 times. Since each group of 10 documents should always be processed in less than 500
    // milliseconds, we expect to hit only iteration-based yields for this query, and no
    // timing-based yields. 40 documents total divided by 10 documents per yield gives us an
    // estimated yield count of 4 yields.
    //
    // Note also that we have a 1-second sleep delay during each yield, and we expect this delay to
    // not change our expectation to hit zero timing-based yields. Timing-based yields only consider
    // time spent during query execution since the last yield; since our sleep delay of 1 second is
    // not during query execution, it should never count towards our 500 millisecond threshold for a
    // timing-based yield (incorrect accounting for timing-based yields was the cause for
    // SERVER-21341).
    for (var i = 0; i < 40; ++i) {
        assert.writeOK(coll.insert({}));
    }
    var explainRes = coll.find().explain("executionStats");
    // We expect 4 yields, but we throw in a fudge factor of 2 for test reliability. We also can
    // use "saveState" calls as a proxy for "number of yields" here, because we expect our entire
    // result set to be returned in a single batch.
    assert.gt(explainRes.executionStats.executionStages.saveState, 4 / 2, tojson(explainRes));
    assert.lt(explainRes.executionStats.executionStages.saveState, 4 * 2, tojson(explainRes));
})();
