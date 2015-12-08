// SERVER-2905 sorting with missing fields

(function() {
    'use strict';

    var t = db.jstests_sorta;
    t.drop();

    assert.writeOK(t.insert({_id: 0, a: MinKey}));
    assert.writeOK(t.save({_id: 3, a: null}));
    assert.writeOK(t.save({_id: 1, a: []}));
    assert.writeOK(t.save({_id: 7, a: [2]}));
    assert.writeOK(t.save({_id: 4}));
    assert.writeOK(t.save({_id: 5, a: null}));
    assert.writeOK(t.save({_id: 2, a: []}));
    assert.writeOK(t.save({_id: 6, a: 1}));
    assert.writeOK(t.insert({_id: 8, a: MaxKey}));

    function sorted(arr) {
        assert.eq(9, arr.length, tojson(arr));
        for (var i = 1; i < arr.length; ++i) {
            assert.lte(arr[i - 1]._id, arr[i]._id);
        }
    }

    sorted(t.find().sort({a: 1}).toArray());
    assert.commandWorked(t.ensureIndex({a: 1}));
    sorted(t.find().sort({a: 1}).hint({a: 1}).toArray());
})();
