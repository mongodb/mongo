// @tags: [
//   assumes_unsharded_collection,
//   requires_fastcount,
//   requires_javascript,
//   requires_non_retryable_commands,
//   sbe_incompatible,
// ]

t = db.jstests_js3;

debug = function(s) {
    // printjson( s );
};

for (z = 0; z < 2; z++) {
    debug(z);

    t.drop();

    if (z > 0) {
        t.ensureIndex({_id: 1});
        t.ensureIndex({i: 1});
    }

    for (i = 0; i < 1000; i++)
        t.save({
            i: i,
            z: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        });

    assert(2 == t.find({
                     $where: function() {
                         return obj.i == 7 || obj.i == 8;
                     }
                 }).length());
    assert.eq(1000, t.count());

    // NPE test
    var ok = false;
    try {
        var x = t.find({
            $where: function() {
                asdf.asdf.f.s.s();
            }
        });
        debug(x.length());
        debug(tojson(x));
    } catch (e) {
        ok = true;
    }
    debug(ok);
    assert(ok);

    t.ensureIndex({z: 1});
    t.ensureIndex({q: 1});

    debug("before indexed find");

    arr = t.find({
               $where: function() {
                   return obj.i == 7 || obj.i == 8;
               }
           }).toArray();
    debug(arr);
    assert.eq(2, arr.length);

    debug("after indexed find");

    for (i = 1000; i < 2000; i++)
        t.save({
            i: i,
            z: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        });

    assert(t.find().count() == 2000);

    assert(t.validate().valid);

    debug("done iter");
}

t.drop();
