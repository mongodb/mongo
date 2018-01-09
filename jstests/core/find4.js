(function() {
    "use strict";

    const coll = db.find4;
    coll.drop();

    assert.writeOK(coll.insert({a: 1123, b: 54332}));

    let o = coll.findOne();
    assert.eq(1123, o.a, "A");
    assert.eq(54332, o.b, "B");
    assert(o._id.str, "C");

    o = coll.findOne({}, {a: 1});
    assert.eq(1123, o.a, "D");
    assert(o._id.str, "E");
    assert(!o.b, "F");

    o = coll.findOne({}, {b: 1});
    assert.eq(54332, o.b, "G");
    assert(o._id.str, "H");
    assert(!o.a, "I");

    assert(coll.drop());

    assert.writeOK(coll.insert({a: 1, b: 1}));
    assert.writeOK(coll.insert({a: 2, b: 2}));
    assert.eq("1-1,2-2",
              coll.find()
                  .sort({a: 1})
                  .map(function(z) {
                      return z.a + "-" + z.b;
                  })
                  .toString());
    assert.eq("1-undefined,2-undefined",
              coll.find({}, {a: 1})
                  .sort({a: 1})
                  .map(function(z) {
                      return z.a + "-" + z.b;
                  })
                  .toString());
}());
