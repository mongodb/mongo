(function() {
    "use strict";

    // Tests for SERVER-20080
    //
    // Verify that various types cannot be invoked as constructors

    var t = db.jstests_type4;
    t.drop();
    t.insert({});
    t.insert({});
    t.insert({});

    var oldReadMode = db.getMerizo().readMode();

    assert.throws(function() {
        (new _rand())();
    }, [], "invoke constructor on natively injected function");

    assert.throws(function() {
        var doc = db.test.findOne();
        new doc();
    }, [], "invoke constructor on BSON");

    assert.throws(function() {
        db.getMerizo().forceReadMode("commands");
        var cursor = t.find();
        cursor.next();

        new cursor._cursor._cursorHandle();
    }, [], "invoke constructor on CursorHandle");

    assert.throws(function() {
        db.getMerizo().forceReadMode("legacy");
        var cursor = t.find();
        cursor.next();

        new cursor._cursor();
    }, [], "invoke constructor on Cursor");

    db.getMerizo().forceReadMode(oldReadMode);
})();
