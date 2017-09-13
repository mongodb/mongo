// tests that $addToSet works in a multi-update.
(function() {
    "use strict";
    var t = db.update_multi5;
    t.drop();

    assert.writeOK(t.insert({path: 'r1', subscribers: [1, 2]}));
    assert.writeOK(t.insert({path: 'r2', subscribers: [3, 4]}));

    var res = assert.writeOK(t.update(
        {}, {$addToSet: {subscribers: 5}}, {upsert: false, multi: true, writeConcern: {w: 1}}));

    assert.eq(res.nMatched, 2, tojson(res));

    t.find().forEach(function(z) {
        assert.eq(3, z.subscribers.length, tojson(z));
    });
})();
