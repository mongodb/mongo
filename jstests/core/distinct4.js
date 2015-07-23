// Vaildate input to distinct command. SERVER-12642

(function() {
    "use strict";

    var t = db.distinct4;

    t.drop();
    t.save({a:null});
    t.save({a:1});
    t.save({a:1});
    t.save({a:2});
    t.save({a:3});

    //first argument should be a string or error

    // from shell helper
    assert.throws( t.distinct, [{a:1}] );

    // from command interface
    assert.commandFailedWithCode(t.runCommand("distinct", {"key": {a: 1}}),
                                 14); // ErrorCodes::TypeMismatch


    //second argument should be a document or error

    // from shell helper
    assert.throws( t.distinct, ['a', '1'] );

    // from command interface
    assert.commandFailedWithCode(t.runCommand("distinct", {"key": "a", "query": "a"}),
                                 14); // ErrorCodes::TypeMismatch



    // empty query clause should not cause error
    assert( t.runCommand( "distinct", { "key" : "a" } ) );

    assert( t.distinct, ['a'] );

})();
