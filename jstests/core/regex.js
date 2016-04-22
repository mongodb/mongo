(function() {
    'use strict';

    var t = db.jstests_regex;

    t.drop();
    t.save({a: "bcd"});
    assert.eq(1, t.count({a: /b/}), "A");
    assert.eq(1, t.count({a: /bc/}), "B");
    assert.eq(1, t.count({a: /bcd/}), "C");
    assert.eq(0, t.count({a: /bcde/}), "D");

    t.drop();
    t.save({a: {b: "cde"}});
    assert.eq(1, t.count({'a.b': /de/}), "E");

    t.drop();
    t.save({a: {b: ["cde"]}});
    assert.eq(1, t.count({'a.b': /de/}), "F");

    t.drop();
    t.save({a: [{b: "cde"}]});
    assert.eq(1, t.count({'a.b': /de/}), "G");

    t.drop();
    t.save({a: [{b: ["cde"]}]});
    assert.eq(1, t.count({'a.b': /de/}), "H");

    // Disallow embedded null bytes when using $regex syntax.
    t.drop();
    assert.throws(function() {
        t.find({a: {$regex: "a\0b", $options: "i"}}).itcount();
    });
    assert.throws(function() {
        t.find({a: {$regex: "ab", $options: "i\0"}}).itcount();
    });
    assert.throws(function() {
        t.find({key: {$regex: 'abcd\0xyz'}}).explain();
    });
})();
