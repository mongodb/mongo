// Integration tests for collation-aware updates.
(function() {
    'use strict';
    var coll = db.coll;

    const caseInsensitive = {collation: {locale: "en_US", strength: 2}};
    const caseSensitive = {collation: {locale: "en_US", strength: 3}};
    const numericOrdering = {collation: {locale: "en_US", numericOrdering: true}};

    // Update modifiers respect collection default collation on simple _id query.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), numericOrdering));
    assert.writeOK(coll.insert({_id: 1, a: "124"}));
    assert.writeOK(coll.update({_id: 1}, {$min: {a: "1234"}}));
    assert.eq(coll.find({a: "124"}).count(), 1);

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

    // $addToSet respects query collation.
    if (db.getMongo().writeMode() === "commands") {
        coll.drop();

        // "foo" == "FOO" (case-insensitive), so set isn't extended.
        assert.writeOK(coll.insert({a: ["foo"]}));
        assert.writeOK(coll.update({}, {$addToSet: {a: "FOO"}}, caseInsensitive));
        var set = coll.findOne().a;
        assert.eq(set.length, 1);

        // "foo" != "FOO" (case-sensitive), so set is extended.
        assert.writeOK(coll.update({}, {$addToSet: {a: "FOO"}}, caseSensitive));
        set = coll.findOne().a;
        assert.eq(set.length, 2);

        coll.drop();

        // $each and $addToSet respect collation
        assert.writeOK(coll.insert({a: ["foo", "bar", "FOO"]}));
        assert.writeOK(
            coll.update({}, {$addToSet: {a: {$each: ["FOO", "BAR", "str"]}}}, caseInsensitive));
        set = coll.findOne().a;
        assert.eq(set.length, 4);
        assert(set.includes("foo"));
        assert(set.includes("FOO"));
        assert(set.includes("bar"));
        assert(set.includes("str"));
    }

    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
    // "foo" == "FOO" (case-insensitive), so set isn't extended.
    assert.writeOK(coll.insert({a: ["foo"]}));
    assert.writeOK(coll.update({}, {$addToSet: {a: "FOO"}}));
    var set = coll.findOne().a;
    assert.eq(set.length, 1);

    // $pull respects query collation.
    if (db.getMongo().writeMode() === "commands") {
        coll.drop();

        // "foo" != "FOO" (case-sensitive), so it is not pulled.
        assert.writeOK(coll.insert({a: ["foo", "FOO"]}));
        assert.writeOK(coll.update({}, {$pull: {a: "foo"}}, caseSensitive));
        var arr = coll.findOne().a;
        assert.eq(arr.length, 1);
        assert(arr.includes("FOO"));

        // "foo" == "FOO" (case-insensitive), so "FOO" is pulled.
        assert.writeOK(coll.update({}, {$pull: {a: "foo"}}, caseInsensitive));
        arr = coll.findOne().a;
        assert.eq(arr.length, 0);

        // collation-aware $pull removes all instances that match.
        coll.drop();
        assert.writeOK(coll.insert({a: ["foo", "FOO"]}));
        assert.writeOK(coll.update({}, {$pull: {a: "foo"}}, caseInsensitive));
        arr = coll.findOne().a;
        assert.eq(arr.length, 0);

        // collation-aware $pull with comparisons removes matching instances.
        coll.drop();

        // "124" > "1234" (case-sensitive), so it is not removed.
        assert.writeOK(coll.insert({a: ["124", "1234"]}));
        assert.writeOK(coll.update({}, {$pull: {a: {$lt: "1234"}}}, caseSensitive));
        arr = coll.findOne().a;
        assert.eq(arr.length, 2);

        // 124 < 1234 (numeric ordering), so it is removed.
        assert.writeOK(coll.update({}, {$pull: {a: {$lt: "1234"}}}, numericOrdering));
        arr = coll.findOne().a;
        assert.eq(arr.length, 1);
        assert(arr.includes("1234"));
    }

    // $pull respects collection default collation.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
    assert.writeOK(coll.insert({a: ["foo", "FOO"]}));
    assert.writeOK(coll.update({}, {$pull: {a: "foo"}}));
    var arr = coll.findOne().a;
    assert.eq(arr.length, 0);

    // $pullAll respects query collation.
    if (db.getMongo().writeMode() === "commands") {
        coll.drop();

        // "foo" != "FOO" (case-sensitive), so no changes are made.
        assert.writeOK(coll.insert({a: ["foo", "bar"]}));
        assert.writeOK(coll.update({}, {$pullAll: {a: ["FOO", "BAR"]}}, caseSensitive));
        var arr = coll.findOne().a;
        assert.eq(arr.length, 2);

        // "foo" == "FOO", "bar" == "BAR" (case-insensitive), so both are removed.
        assert.writeOK(coll.update({}, {$pullAll: {a: ["FOO", "BAR"]}}, caseInsensitive));
        arr = coll.findOne().a;
        assert.eq(arr.length, 0);
    }

    // $pullAll respects collection default collation.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
    assert.writeOK(coll.insert({a: ["foo", "bar"]}));
    assert.writeOK(coll.update({}, {$pullAll: {a: ["FOO", "BAR"]}}));
    var arr = coll.findOne().a;
    assert.eq(arr.length, 0);

    // $push with $sort respects query collation.
    if (db.getMongo().writeMode() === "commands") {
        coll.drop();

        // "1230" < "1234" < "124" (case-sensitive)
        assert.writeOK(coll.insert({a: ["1234", "124"]}));
        assert.writeOK(coll.update({}, {$push: {a: {$each: ["1230"], $sort: 1}}}, caseSensitive));
        var arr = coll.findOne().a;
        assert.eq(arr.length, 3);
        assert.eq(arr[0], "1230");
        assert.eq(arr[1], "1234");
        assert.eq(arr[2], "124");

        // "124" < "1230" < "1234" (numeric ordering)
        coll.drop();
        assert.writeOK(coll.insert({a: ["1234", "124"]}));
        assert.writeOK(coll.update({}, {$push: {a: {$each: ["1230"], $sort: 1}}}, numericOrdering));
        arr = coll.findOne().a;
        assert.eq(arr.length, 3);
        assert.eq(arr[0], "124");
        assert.eq(arr[1], "1230");
        assert.eq(arr[2], "1234");
    }

    // $push with $sort respects collection default collation.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), numericOrdering));
    assert.writeOK(coll.insert({a: ["1234", "124"]}));
    assert.writeOK(coll.update({}, {$push: {a: {$each: ["1230"], $sort: 1}}}));
    var arr = coll.findOne().a;
    assert.eq(arr.length, 3);
    assert.eq(arr[0], "124");
    assert.eq(arr[1], "1230");
    assert.eq(arr[2], "1234");

    // $ positional operator respects query collation on $set.
    if (db.getMongo().writeMode() === "commands") {
        coll.drop();

        // "foo" != "FOO" (case-sensitive) so no update occurs.
        assert.writeOK(coll.insert({a: ["foo", "FOO"]}));
        assert.writeOK(coll.update({a: "FOO"}, {$set: {"a.$": "FOO"}}, caseSensitive));
        var arr = coll.findOne().a;
        assert.eq(arr.length, 2);
        assert.eq(arr[0], "foo");
        assert.eq(arr[1], "FOO");

        // "foo" == "FOO" (case-insensitive) so no update occurs.
        assert.writeOK(coll.insert({a: ["foo", "FOO"]}));
        assert.writeOK(coll.update({a: "FOO"}, {$set: {"a.$": "FOO"}}, caseInsensitive));
        var arr = coll.findOne().a;
        assert.eq(arr.length, 2);
        assert.eq(arr[0], "FOO");
        assert.eq(arr[1], "FOO");
    }

    // $ positional operator respects collection default collation on $set.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
    assert.writeOK(coll.insert({a: ["foo", "FOO"]}));
    assert.writeOK(coll.update({a: "FOO"}, {$set: {"a.$": "FOO"}}));
    var arr = coll.findOne().a;
    assert.eq(arr.length, 2);
    assert.eq(arr[0], "FOO");
    assert.eq(arr[1], "FOO");
})();
