// Test partial index creation and drops.

(function() {
    "use strict";
    var coll = db.index_partial_create_drop;

    var getNumKeys = function(idxName) {
        var res = assert.commandWorked(coll.validate(true));
        var kpi;

        var isShardedNS = res.hasOwnProperty('raw');
        if (isShardedNS) {
            kpi = res.raw[Object.getOwnPropertyNames(res.raw)[0]].keysPerIndex;
        } else {
            kpi = res.keysPerIndex;
        }
        return kpi[coll.getFullName() + ".$" + idxName];
    };

    coll.drop();

    // Check bad filter spec on create.
    assert.commandFailed(coll.ensureIndex({x: 1}, {partialFilterExpression: 5}));
    assert.commandFailed(coll.ensureIndex({x: 1}, {partialFilterExpression: {x: {$asdasd: 3}}}));
    assert.commandFailed(coll.ensureIndex({x: 1}, {partialFilterExpression: {$and: 5}}));
    assert.commandFailed(coll.ensureIndex({x: 1}, {partialFilterExpression: {x: /abc/}}));
    assert.commandFailed(coll.ensureIndex({x: 1}, {
        partialFilterExpression:
            {$and: [{$and: [{x: {$lt: 2}}, {x: {$gt: 0}}]}, {x: {$exists: true}}]}
    }));

    for (var i = 0; i < 10; i++) {
        assert.writeOK(coll.insert({x: i, a: i}));
    }

    // Create partial index.
    assert.commandWorked(coll.ensureIndex({x: 1}, {partialFilterExpression: {a: {$lt: 5}}}));
    assert.eq(5, getNumKeys("x_1"));
    assert.commandWorked(coll.dropIndex({x: 1}));
    assert.eq(1, coll.getIndexes().length);

    // Create partial index in background.
    assert.commandWorked(
        coll.ensureIndex({x: 1}, {background: true, partialFilterExpression: {a: {$lt: 5}}}));
    assert.eq(5, getNumKeys("x_1"));
    assert.commandWorked(coll.dropIndex({x: 1}));
    assert.eq(1, coll.getIndexes().length);

    // Create complete index, same key as previous indexes.
    assert.commandWorked(coll.ensureIndex({x: 1}));
    assert.eq(10, getNumKeys("x_1"));
    assert.commandWorked(coll.dropIndex({x: 1}));
    assert.eq(1, coll.getIndexes().length);

    // Partial indexes can't also be sparse indexes.
    assert.commandFailed(coll.ensureIndex({x: 1}, {partialFilterExpression: {a: 1}, sparse: true}));
    assert.commandFailed(coll.ensureIndex({x: 1}, {partialFilterExpression: {a: 1}, sparse: 1}));
    assert.commandWorked(
        coll.ensureIndex({x: 1}, {partialFilterExpression: {a: 1}, sparse: false}));
    assert.eq(2, coll.getIndexes().length);
    assert.commandWorked(coll.dropIndex({x: 1}));
    assert.eq(1, coll.getIndexes().length);

    // SERVER-18858: Verify that query compatible w/ partial index succeeds after index drop.
    assert.commandWorked(coll.ensureIndex({x: 1}, {partialFilterExpression: {a: {$lt: 5}}}));
    assert.commandWorked(coll.dropIndex({x: 1}));
    assert.eq(1, coll.find({x: 0, a: 0}).itcount());
})();
