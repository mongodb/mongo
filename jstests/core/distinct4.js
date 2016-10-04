// Vaildate input to distinct command. SERVER-12642

(function() {
    "use strict";

    var t = db.distinct4;

    t.drop();
    t.save({a: null});
    t.save({a: 1});
    t.save({a: 1});
    t.save({a: 2});
    t.save({a: 3});

    // first argument should be a string or error

    // from shell helper
    assert.throws(function() {
        t.distinct({a: 1});
    });

    // from command interface
    assert.commandFailedWithCode(t.runCommand("distinct", {"key": {a: 1}}),
                                 ErrorCodes.TypeMismatch);

    // second argument should be a document or error

    // from shell helper
    assert.throws(function() {
        t.distinct('a', '1');
    });

    // from command interface
    assert.commandFailedWithCode(t.runCommand("distinct", {"key": "a", "query": "a"}),
                                 ErrorCodes.TypeMismatch);

    // empty query clause should not cause error

    // from shell helper
    var a = assert.doesNotThrow(function() {
        return t.distinct('a');
    });
    // [ null, 1, 2, 3 ]
    assert.eq(4, a.length, tojson(a));
    assert.contains(null, a);
    assert.contains(1, a);
    assert.contains(2, a);
    assert.contains(3, a);

    // from command interface
    assert.commandWorked(t.runCommand("distinct", {"key": "a"}));

})();
