// @tags: [
//   # Uses $where operator
//   requires_scripting,
//   requires_getmore,
// ]

let t = db.jstests_js4;
t.drop();

let real = {a: 1, b: "abc", c: /abc/i, d: new Date(111911100111), e: null, f: true};

t.save(real);

assert.eq("/abc/i", real.c.toString(), "regex 1");

var cursor = t.find({
    $where: function () {
        /* eslint-disable */
        assert.eq(7, Object.keySet(obj).length, "A");
        assert.eq(1, obj.a, "B");
        assert.eq("abc", obj.b, "C");
        assert.eq("/abc/i", obj.c.toString(), "D");
        assert.eq(111911100111, obj.d.getTime(), "E");
        assert(obj.f, "F");
        assert(!obj.e, "G");
        /* eslint-enable */

        return true;
    },
});
assert.eq(1, cursor.toArray().length);
assert.eq("abc", cursor[0].b);

// ---

t.drop();
t.save({a: 2, b: {c: 7, d: "d is good"}});
var cursor = t.find({
    $where: function () {
        /* eslint-disable */
        assert.eq(3, Object.keySet(obj).length);
        assert.eq(2, obj.a);
        assert.eq(7, obj.b.c);
        assert.eq("d is good", obj.b.d);
        /* eslint-enable */

        return true;
    },
});
assert.eq(1, cursor.toArray().length);

assert(t.validate().valid);
