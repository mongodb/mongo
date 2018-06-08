// Ensure that multi-update and multi-remove operations yield regularly.
// @tags: [requires_profiling]
(function() {
    'use strict';

    function countOpYields(coll, op) {
        const profileEntry = coll.getDB()
                                 .system.profile.find({ns: coll.getFullName()})
                                 .sort({$natural: -1})
                                 .limit(1)
                                 .next();
        assert.eq(profileEntry.op, op);
        return profileEntry.numYield;
    }

    const nDocsToInsert = 300;
    const worksPerYield = 50;

    // Start a mongod that will yield every 50 work cycles.
    const mongod = MongoRunner.runMongod({
        setParameter: `internalQueryExecYieldIterations=${worksPerYield}`,
        profile: 2,
    });
    assert.neq(null, mongod, 'mongod was unable to start up');

    const coll = mongod.getDB('test').yield_during_writes;
    coll.drop();

    for (let i = 0; i < nDocsToInsert; i++) {
        assert.writeOK(coll.insert({_id: i}));
    }

    // A multi-update doing a collection scan should yield about nDocsToInsert / worksPerYield
    // times.
    assert.writeOK(coll.update({}, {$inc: {counter: 1}}, {multi: true}));
    assert.gt(countOpYields(coll, 'update'), (nDocsToInsert / worksPerYield) - 2);

    // Likewise, a multi-remove should also yield approximately every worksPerYield documents.
    assert.writeOK(coll.remove({}, {multi: true}));
    assert.gt(countOpYields(coll, 'remove'), (nDocsToInsert / worksPerYield) - 2);

    MongoRunner.stopMongod(mongod);
})();
