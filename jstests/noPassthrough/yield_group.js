/**
 * Test that the "group" command yields periodically (SERVER-1395).
 */
(function() {
    'use strict';

    const nDocsToInsert = 300;
    const worksPerYield = 50;

    // Start a mongod that will yield every 50 work cycles.
    var conn =
        MongoRunner.runMongod({setParameter: `internalQueryExecYieldIterations=${worksPerYield}`});
    assert.neq(null, conn, 'mongod was unable to start up');

    var coll = conn.getDB('test').yield_group;

    var bulk = coll.initializeUnorderedBulkOp();
    for (var i = 0; i < nDocsToInsert; ++i) {
        bulk.insert({_id: i});
    }

    var res = bulk.execute();
    assert.writeOK(res);
    assert.eq(nDocsToInsert, res.nInserted, tojson(res));

    // A "group" command performing a collection scan should yield approximately
    // nDocsToInsert / worksPerYield times.
    var explain =
        coll.explain('executionStats').group({key: {_id: 1}, reduce: function() {}, initial: {}});
    var numYields = explain.executionStats.executionStages.saveState;
    assert.gt(numYields, (nDocsToInsert / worksPerYield) - 2, tojson(explain));
})();
