// Integration tests for collation-aware updates.
(function() {
    'use strict';
    var coll = db.coll;

    const caseSensitive = {collation: {locale: "en_US", strength: 3}};
    const numericOrdering = {collation: {locale: "en_US", numericOrdering: true}};

    // $min respects query collation.
    if (db.getMongo().writeMode() === "commands") {
        coll.drop();

        // 1234 > 124, so no change should occur.
        assert.writeOK(coll.insert({a: "124"}));
        assert.writeOK(coll.update({a: "124"}, {$min: {a: "1234"}}, numericOrdering));
        assert.eq(coll.find({a: "124"}).count(), 1);

        // "1234" < "124" (non-numeric ordering), so an update should occur.
        assert.writeOK(coll.update({a: "124"}, {$min: {a: "1234"}}, caseSensitive));
        assert.eq(coll.find({a: "1234"}).count(), 1);
    }

    // $min respects collection default collation.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), numericOrdering));
    assert.writeOK(coll.insert({a: "124"}));
    assert.writeOK(coll.update({a: "124"}, {$min: {a: "1234"}}));
    assert.eq(coll.find({a: "124"}).count(), 1);

    // $max respects query collation.
    if (db.getMongo().writeMode() === "commands") {
        coll.drop();

        // "1234" < "124", so an update should not occur.
        assert.writeOK(coll.insert({a: "124"}));
        assert.writeOK(coll.update({a: "124"}, {$max: {a: "1234"}}, caseSensitive));
        assert.eq(coll.find({a: "124"}).count(), 1);

        // 1234 > 124, so an update should occur.
        assert.writeOK(coll.update({a: "124"}, {$max: {a: "1234"}}, numericOrdering));
        assert.eq(coll.find({a: "1234"}).count(), 1);
    }

    // $max respects collection default collation.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), numericOrdering));
    assert.writeOK(coll.insert({a: "124"}));
    assert.writeOK(coll.update({a: "124"}, {$max: {a: "1234"}}));
    assert.eq(coll.find({a: "1234"}).count(), 1);
})();
