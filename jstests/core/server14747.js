// Validation test for SERVER-14747. Note that the issue under test is a memory leak, so this
// test would only be expected to fail when run under address sanitizer.

(function() {

    "use strict";
    var t = db.jstests_server14747;

    t.drop();
    t.ensureIndex({a: 1, b: 1});
    t.ensureIndex({a: 1, c: 1});
    t.insert({a: 1});
    for (var i = 0; i < 10; i++) {
        t.find({a: 1}).explain(true);
    }

}());
