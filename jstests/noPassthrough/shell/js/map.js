import {describe, it} from "jstests/libs/mochalite.js";

describe("Map shims and polyfills", function() {
    describe("built-in Map", function() {
        it("should be able to create and compare Maps", function() {
            let m = new Map();
            assert.eq(m, m);
            assert.eq(m, new Map());

            let m1 = new Map();
            m1.set("key", 1);
            assert.eq(m1, m1);
            assert.neq(m1, m);

            let m2 = new Map();
            m2.set("key", 1);
            assert.eq(m1, m2);
        });

        it("converts to JSON", function() {
            let m = new Map();
            assert.eq(Map.tojson(m), 'new Map([ ])');

            m.set("key1", 1);
            m.set("key2", 2);
            assert.eq(Map.tojson(m, '', true), 'new Map([ [ "key1", 1 ], [ "key2", 2 ] ])');
        });
    });

    describe("BSONAwareMap", function() {
        it("should be able to create and compare BSONAwareMaps", function() {
            let m = new BSONAwareMap();
            assert.eq(m, m);
            assert.eq(m, new BSONAwareMap());
        });

        it("should put and get", function() {
            let m1 = new BSONAwareMap();
            assert.isnull(m1.get("key"));

            m1.put("key", 1);
            assert.eq(m1.get("key"), 1);

            assert.eq(m1, m1);
            assert.neq(m1, new Map());

            let m2 = new BSONAwareMap();
            m2.put("key", 1);
            assert.eq(m1, m2);

            m2.put("key", 2);
            assert.eq(m2.get("key"), 2);
        });

        it("should put and get with object keys", function() {
            let m = new BSONAwareMap();

            m.put({a: 1}, 17);
            assert.eq(m.get({a: 1}), 17);
            assert.isnull(m.get({b: 1}));
            assert.isnull(m.get({b: 2}));

            m.put({a: 1, b: 1}, 18);
            assert.eq(m.get({a: 1, b: 1}), 18);

            // order matters
            assert.isnull(m.get({b: 1, a: 1}));
        });

        it("hash", function() {
            let h, err;

            // nullish
            h = BSONAwareMap.hash();
            assert.eq(h, undefined);

            h = BSONAwareMap.hash(undefined);
            assert.eq(h, undefined);

            h = BSONAwareMap.hash(null);
            assert.eq(h, null);

            // string
            h = BSONAwareMap.hash("hello");
            assert.eq(h, "hello");

            // number
            h = BSONAwareMap.hash(7);
            assert.eq(h, 7);

            // object
            h = BSONAwareMap.hash({a: 5, b: "foo"});
            assert.eq(h, "a5bfoo");

            // array
            h = BSONAwareMap.hash([5, "foo"]);
            assert.eq(h, "051foo");

            // invalid
            err = assert.throws(() => {
                BSONAwareMap.hash(function() {});
            });
            assert.eq(err.message, "can't hash : function");

            // boolean
            err = assert.throws(() => {
                BSONAwareMap.hash(true);
            });
            assert.eq(err.message, "can't hash : boolean");

            // this happens to work, but is assymetric with true
            h = BSONAwareMap.hash(false);
            assert.eq(h, false);
        });

        it("values", function() {
            let m = new BSONAwareMap();
            m.put("key1", 1);
            m.put("key2", "foo");

            let values = m.values();
            assert.eq(values, [1, "foo"]);
        });
    });
});
