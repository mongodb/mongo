/**
 * SERVER-20529: Ensure capped document sizes do not change
 */
(function() {
    'use strict';
    var t = db.cannot_change_capped_size;
    t.drop();
    assert.commandWorked(db.createCollection(t.getName(), {capped: true, size: 1024}));

    for (var j = 1; j <= 10; j++) {
        assert.writeOK(t.insert({_id: j, s: "Hello, World!"}));
    }

    assert.writeOK(t.update({_id: 3}, {s: "Hello, Mongo!"})); // Mongo is same length as World
    assert.writeError(t.update({_id: 3}, {$set: {s: "Hello!"}}));
    assert.writeError(t.update({_id: 10}, {}));
    assert.writeError(t.update({_id: 10}, {s: "Hello, World!!!"}));
})();
