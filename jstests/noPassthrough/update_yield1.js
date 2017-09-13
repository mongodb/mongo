// Ensure that multi-update operations yield regularly.
// Unless they are $isolated and they shouldn't yield at all.
(function() {
    'use strict';

    function countUpdateYields(coll) {
        var profileEntry = coll.getDB()
                               .system.profile.find({ns: coll.getFullName()})
                               .sort({$natural: -1})
                               .limit(1)
                               .next();
        printjson(profileEntry);
        assert.eq(profileEntry.op, 'update');
        return profileEntry.numYield;
    }

    var explain;
    var yieldCount;

    const nDocsToInsert = 300;
    const worksPerYield = 50;

    // Start a mongod that will yield every 50 work cycles.
    var mongod = MongoRunner.runMongod({
        setParameter: `internalQueryExecYieldIterations=${worksPerYield}`,
        profile: 2,
    });
    assert.neq(null, mongod, 'mongod was unable to start up');

    var coll = mongod.getDB('test').update_yield1;
    coll.drop();

    for (var i = 0; i < nDocsToInsert; i++) {
        assert.writeOK(coll.insert({_id: i}));
    }

    // A multi-update doing a collection scan should yield about nDocsToInsert / worksPerYield
    // times.
    assert.writeOK(coll.update({}, {$inc: {counter: 1}}, {multi: true}));
    assert.gt(countUpdateYields(coll), (nDocsToInsert / worksPerYield) - 2);

    // A multi-update shouldn't yield if it has $isolated.
    assert.writeOK(coll.update({$isolated: true}, {$inc: {counter: 1}}, {multi: true}));
    assert.eq(countUpdateYields(coll), 0, 'yielded during $isolated multi-update');
})();
