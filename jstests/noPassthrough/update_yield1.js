// Ensure that multi-update operations yield regularly.
(function() {
    'use strict';

    var explain;
    var yieldCount;

    const nDocsToInsert = 300;
    const worksPerYield = 50;

    // Start a mongod that will yield every 50 work cycles.
    // clang-format off
    var mongod = MongoRunner.runMongod({
        setParameter: `internalQueryExecYieldIterations=${worksPerYield}`
    });
    // clang-format on
    assert.neq(null, mongod, 'mongod was unable to start up');

    var coll = mongod.getDB('test').update_yield1;
    coll.drop();

    for (var i = 0; i < nDocsToInsert; i++) {
        assert.writeOK(coll.insert({_id: i}));
    }

    // A multi-update doing a collection scan should yield about nDocsToInsert / worksPerYield
    // times.
    explain = coll.explain('executionStats').update({}, {$inc: {counter: 1}}, {multi: true});
    assert.commandWorked(explain);
    yieldCount = explain.executionStats.executionStages.saveState;
    assert.gt(yieldCount, (nDocsToInsert / worksPerYield) - 2);

    // A multi-update shouldn't yield if it has $isolated.
    explain = coll.explain('executionStats')
                  .update({$isolated: true}, {$inc: {counter: 1}}, {multi: true});
    assert.commandWorked(explain);
    yieldCount = explain.executionStats.executionStages.saveState;
    assert.eq(yieldCount, 0, 'yielded during $isolated multi-update');
})();
