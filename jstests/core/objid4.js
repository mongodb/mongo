

o = new ObjectId();
assert(o.str);

a = new ObjectId(o.str);
assert.eq(o.str, a.str);
assert.eq(a.str, a.str.toString());

b = ObjectId(o.str);
assert.eq(o.str, b.str);
assert.eq(b.str, b.str.toString());

assert.throws(function(z) {
    return new ObjectId("a");
});
assert.throws(function(z) {
    return new ObjectId("12345678901234567890123z");
});
