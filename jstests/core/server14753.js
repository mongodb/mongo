// Validation test for SERVER-14753. Note that the issue under test is a memory leak, so this
// test would only be expected to fail when run under address sanitizer.

(function() {

    "use strict";
    var t = db.jstests_server14753;

    t.drop();
    t.ensureIndex({a: 1});
    t.ensureIndex({b: 1});
    for (var i = 0; i < 20; i++) {
        t.insert({b: i});
    }
    for (var i = 0; i < 20; i++) {
        t.find({b: 1}).sort({a: 1}).next();
    }

}());
