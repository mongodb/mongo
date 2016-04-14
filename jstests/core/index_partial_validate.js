// Tests that the validate command works with partial indexes and a document that does not have
// an indexed field. For details, see SERVER-23730.
'use strict';

(function() {
    var t = db.index_partial_validate;
    t.drop();

    var res = t.ensureIndex({a: 1}, {partialFilterExpression: {a: {$lte: 1}}});
    assert.commandWorked(res);

    res = t.ensureIndex({b: 1});
    assert.commandWorked(res);

    res = t.insert({non_indexed_field: 'x'});
    assert.writeOK(res);

    res = t.validate(true);
    assert.commandWorked(res);
    assert(res.valid, 'Validate failed with response:\n' + tojson(res));
})();
