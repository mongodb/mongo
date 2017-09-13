// This checks to make sure that cursors with a limit get killed by the shell
// after all their results have been returned. See SERVER-17792.
(function() {
    "use strict";

    var t = db.cursor_limit_test;
    t.drop();
    var pre = db.serverStatus().metrics.cursor.open.total;

    for (var i = 1; i <= 5; i++) {
        t.save({a: i});
    }

    var c = t.find().limit(3);
    while (c.hasNext()) {
        var v = c.next();
    }

    assert.eq(pre, db.serverStatus().metrics.cursor.open.total);
    t.drop();
}());
