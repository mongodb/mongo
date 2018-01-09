// @tags: [requires_non_retryable_writes]

(function() {
    "use strict";

    const coll = db.jstests_not2;
    coll.drop();

    function check(query, expected, size) {
        if (size === undefined) {
            size = 1;
        }
        assert.eq(size, coll.find(query).itcount(), tojson(query));
        if (size > 0) {
            const cursor = coll.find(query).sort({i: 1});
            assert.eq(expected, cursor.toArray()[0].i, tojson(query));
        }
    }

    function fail(query) {
        assert.throws(() => coll.find(query).itcount());
    }

    function doTest() {
        assert.writeOK(coll.remove({}));

        assert.writeOK(coll.insert({i: "a"}));
        assert.writeOK(coll.insert({i: "b"}));

        // TODO SERVER-12735: We currently do not handle double negatives during query
        // canonicalization.
        fail({i: {$not: {$not: "a"}}});
        check({i: {$not: {$not: {$gt: "a"}}}}, "b");

        fail({i: {$not: "a"}});
        fail({i: {$not: {$ref: "foo"}}});
        fail({i: {$not: {}}});
        check({i: {$gt: "a"}}, "b");
        check({i: {$not: {$gt: "a"}}}, "a");
        check({i: {$not: {$ne: "a"}}}, "a");
        check({i: {$not: {$gte: "b"}}}, "a");
        check({i: {$exists: true}}, "a", 2);
        check({i: {$not: {$exists: true}}}, "", 0);
        check({j: {$not: {$exists: false}}}, "", 0);
        check({j: {$not: {$exists: true}}}, "a", 2);
        check({i: {$not: {$in: ["a"]}}}, "b");
        check({i: {$not: {$in: ["a", "b"]}}}, "", 0);
        check({i: {$not: {$in: ["g"]}}}, "a", 2);
        check({i: {$not: {$nin: ["a"]}}}, "a");
        check({i: {$not: /a/}}, "b");
        check({i: {$not: /(a|b)/}}, "", 0);
        check({i: {$not: /a/, $regex: "a"}}, "", 0);
        check({i: {$not: /aa/}}, "a", 2);
        fail({i: {$not: {$regex: "a"}}});
        fail({i: {$not: {$options: "a"}}});
        check({i: {$type: 2}}, "a", 2);
        check({i: {$not: {$type: 1}}}, "a", 2);
        check({i: {$not: {$type: 2}}}, "", 0);

        assert.writeOK(coll.remove({}));
        assert.writeOK(coll.insert({i: 1}));
        check({i: {$not: {$mod: [5, 1]}}}, null, 0);
        check({i: {$mod: [5, 2]}}, null, 0);
        check({i: {$not: {$mod: [5, 2]}}}, 1, 1);

        assert.writeOK(coll.remove({}));
        assert.writeOK(coll.insert({i: ["a", "b"]}));
        check({i: {$not: {$size: 2}}}, null, 0);
        check({i: {$not: {$size: 3}}}, ["a", "b"]);
        check({i: {$not: {$gt: "a"}}}, null, 0);
        check({i: {$not: {$gt: "c"}}}, ["a", "b"]);
        check({i: {$not: {$all: ["a", "b"]}}}, null, 0);
        check({i: {$not: {$all: ["c"]}}}, ["a", "b"]);

        assert.writeOK(coll.remove({}));
        assert.writeOK(coll.insert({i: [{j: "a"}]}));
        assert.writeOK(coll.insert({i: [{j: "b"}]}));
        check({i: {$not: {$elemMatch: {j: "a"}}}}, [{j: "b"}]);
        check({i: {$not: {$elemMatch: {j: "f"}}}}, [{j: "a"}], 2);
    }

    // Run the test without any index.
    doTest();

    // Run the test with an index present.
    assert.commandWorked(coll.ensureIndex({i: 1}));
    doTest();
}());
