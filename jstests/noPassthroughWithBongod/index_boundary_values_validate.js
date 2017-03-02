// Tests that the validate command works with MinKey and MaxKey.

'use strict';

(function() {
    var t = db.index_boundary_values_validate;
    t.drop();

    assert.writeOK(t.insert({a: MaxKey, b: MaxKey}));
    assert.writeOK(t.insert({a: MaxKey, b: MinKey}));
    assert.writeOK(t.insert({a: MinKey, b: MaxKey}));
    assert.writeOK(t.insert({a: MinKey, b: MinKey}));

    assert.writeOK(t.insert({a: {}}));
    assert.writeOK(t.insert({b: {}}));
    assert.writeOK(t.insert({unindexed_field: {}}));
    assert.writeOK(t.insert({a: {}, b: {}}));

    assert.commandWorked(t.createIndex({a: 1, b: 1}));
    assert.commandWorked(t.createIndex({a: 1, b: -1}));
    assert.commandWorked(t.createIndex({a: -1, b: 1}));
    assert.commandWorked(t.createIndex({a: -1, b: -1}));

    var res = t.validate(true);
    assert.commandWorked(res);

    assert.eq(
        res.nrecords, 8, 'the collection had an unexpected number of records:\n' + tojson(res));
    assert.eq(
        res.nIndexes, 5, 'the collection had an unexpected number of indexes:\n' + tojson(res));
    assert.eq(res.valid, true, 'the collection failed validation:\n' + tojson(res));
})();
