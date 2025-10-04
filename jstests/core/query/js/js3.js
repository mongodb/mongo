// @tags: [
//   assumes_unsharded_collection,
//   requires_fastcount,
//   requires_javascript,
//   requires_non_retryable_commands,
//   # Uses $where operator
//   requires_scripting,
//   requires_getmore,
// ]

let t = db.jstests_js3;

let debug = function (s) {
    // printjson( s );
};

for (let z = 0; z < 2; z++) {
    debug(z);

    t.drop();

    if (z > 0) {
        t.createIndex({_id: 1});
        t.createIndex({i: 1});
    }

    for (let i = 0; i < 1000; i++)
        t.save({
            i: i,
            z: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        });

    assert(
        2 ==
            t
                .find({
                    $where: function () {
                        // eslint-disable-next-line
                        return obj.i == 7 || obj.i == 8;
                    },
                })
                .length(),
    );
    assert.eq(1000, t.count());

    // NPE test
    let ok = false;
    try {
        let x = t.find({
            $where: function () {
                // eslint-disable-next-line
                asdf.asdf.f.s.s();
            },
        });
        debug(x.length());
        debug(tojson(x));
    } catch (e) {
        ok = true;
    }
    debug(ok);
    assert(ok);

    t.createIndex({z: 1});
    t.createIndex({q: 1});

    debug("before indexed find");

    let arr = t
        .find({
            $where: function () {
                // eslint-disable-next-line
                return obj.i == 7 || obj.i == 8;
            },
        })
        .toArray();
    debug(arr);
    assert.eq(2, arr.length);

    debug("after indexed find");

    for (let i = 1000; i < 2000; i++)
        t.save({
            i: i,
            z: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        });

    assert(t.find().count() == 2000);

    assert(t.validate().valid);

    debug("done iter");
}

t.drop();
