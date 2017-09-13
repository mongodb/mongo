
function basic1(key, lookup, shouldFail) {
    var m = new Map();
    m.put(key, 17);

    var out = m.get(lookup || key);

    if (!shouldFail) {
        assert.eq(17, out, "basic1 missing: " + tojson(key));
    } else {
        assert.isnull(out, "basic1 not missing: " + tojson(key));
    }
}

basic1(6);
basic1(new Date());
basic1("eliot");
basic1({a: 1});
basic1({a: 1, b: 1});
basic1({a: 1}, {b: 1}, true);
basic1({a: 1, b: 1}, {b: 1, a: 1}, true);
basic1({a: 1}, {a: 2}, true);
