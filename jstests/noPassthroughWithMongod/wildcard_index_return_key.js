/**
 * Tests that $** indexes works with returnKey option.
 * TODO: SERVER-36198: Move this test back to jstests/core/
 */
(function() {
    'use strict';

    load("jstests/aggregation/extras/utils.js");

    const coll = db.wildcard_return_key;
    coll.drop();

    const assertArrayEq = (l, r) => assert(arrayEq(l, r), tojson(l) + " != " + tojson(r));
    const assertArrayNotEq = (l, r) => assert(!arrayEq(l, r), tojson(l) + " == " + tojson(r));

    // Required in order to build $** indexes.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryAllowAllPathsIndexes: true}));

    try {
        assert.commandWorked(coll.createIndex({"$**": 1}));

        assert.commandWorked(coll.insert({a: 1, b: 2, c: {d: 2, e: 1}}));
        assert.commandWorked(coll.insert({a: 2, b: 2, c: {d: 1, e: 2}}));
        assert.commandWorked(coll.insert({a: 2, b: 1, c: {d: 2, e: 2}}));
        assert.commandWorked(coll.insert({a: 1, b: 1, c: {e: 2}}));

        // $** index return key with one field argument.
        assertArrayEq(coll.find({a: 1}).returnKey().toArray(),
                      [{"$_path": "a", a: 1}, {"$_path": "a", a: 1}]);

        // $** index return key with dot path argument.
        assertArrayEq(coll.find({"c.e": 1}).returnKey().toArray(), [{"$_path": "c.e", "c.e": 1}]);

        assert.commandWorked(coll.createIndex({"a": 1}));

        // $** index return key with competing regular index.
        assertArrayEq(coll.find({a: 1}).hint({"$**": 1}).returnKey().toArray(),
                      [{"$_path": "a", a: 1}, {"$_path": "a", a: 1}]);

        assert.commandWorked(coll.createIndex({"a": 1, "b": 1}));

        // $** index return key with competing compound index.
        assertArrayNotEq(coll.find({a: 1, b: 1}).hint({"$**": 1}).returnKey().toArray(),
                         [{a: 1, b: 1}]);

        assert.commandWorked(coll.insert({a: 2, b: 2, c: {e: 2}, f: [1, 2, 3]}));
        assert.commandWorked(coll.insert({a: 2, b: 2, c: {e: 2}, g: [{h: 1}, {i: 2}]}));

        // Multikey path $** index return key.
        assertArrayEq(coll.find({f: 1}).returnKey().toArray(), [{"$_path": "f", f: 1}]);

        // Multikey subobject $** index return key.
        assertArrayEq(coll.find({"g.h": 1}).returnKey().toArray(), [{"$_path": "g.h", "g.h": 1}]);

        assert.commandWorked(coll.dropIndexes());
        assert.commandWorked(coll.createIndex({"c.$**": 1}));

        // Path specified $** index return key.
        assertArrayEq(coll.find({"c.d": 1}).returnKey().toArray(), [{"$_path": "c.d", "c.d": 1}]);

        // Path specified $** index return key with irrelevant query. We expect this query to be
        // answered with a collection scan, in which case returnKey is expected to return empty
        // objects.
        assertArrayEq(coll.find({a: 1, b: 1}).returnKey().toArray(), [{}]);
    } finally {
        // Disable $** indexes once the tests have either completed or failed.
        db.adminCommand({setParameter: 1, internalQueryAllowAllPathsIndexes: false});
    }
})();