// Write ops tests for partial indexes.

(function() {
    "use strict";
    var coll = db.index_partial_write_ops;

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

    // Create partial index.
    assert.commandWorked(coll.ensureIndex({x: 1}, {partialFilterExpression: {a: 1}}));

    assert.writeOK(coll.insert({_id: 1, x: 5, a: 2, b: 1}));  // Not in index.
    assert.writeOK(coll.insert({_id: 2, x: 6, a: 1, b: 1}));  // In index.

    assert.eq(1, getNumKeys("x_1"));

    // Move into partial index, then back out.
    assert.writeOK(coll.update({_id: 1}, {$set: {a: 1}}));
    assert.eq(2, getNumKeys("x_1"));

    assert.writeOK(coll.update({_id: 1}, {$set: {a: 2}}));
    assert.eq(1, getNumKeys("x_1"));

    // Bit blip doc in partial index, and out of partial index.
    assert.writeOK(coll.update({_id: 2}, {$set: {b: 2}}));
    assert.eq(1, getNumKeys("x_1"));

    assert.writeOK(coll.update({_id: 1}, {$set: {b: 2}}));
    assert.eq(1, getNumKeys("x_1"));

    var array = [];
    for (var i = 0; i < 2048; i++) {
        array.push({arbitrary: i});
    }

    // Update that causes record relocation.
    assert.writeOK(coll.update({_id: 2}, {$set: {b: array}}));
    assert.eq(1, getNumKeys("x_1"));

    assert.writeOK(coll.update({_id: 1}, {$set: {b: array}}));
    assert.eq(1, getNumKeys("x_1"));

    // Delete that doesn't affect partial index.
    assert.writeOK(coll.remove({x: 5}));
    assert.eq(1, getNumKeys("x_1"));

    // Delete that does affect partial index.
    assert.writeOK(coll.remove({x: 6}));
    assert.eq(0, getNumKeys("x_1"));
})();
