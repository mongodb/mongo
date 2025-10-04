// Test different directions for compound indexes

function eq(one, two) {
    assert.eq(one.a, two.a);
    assert.eq(one.b, two.b);
}

function check(indexed) {
    let hint;
    if (indexed) {
        hint = {a: 1, b: -1};
    } else {
        hint = {$natural: 1};
    }

    let f = r.find().sort({a: 1, b: 1}).hint(hint);
    eq(z[0], f[0]);
    eq(z[1], f[1]);
    eq(z[2], f[2]);
    eq(z[3], f[3]);

    f = r.find().sort({a: 1, b: -1}).hint(hint);
    eq(z[1], f[0]);
    eq(z[0], f[1]);
    eq(z[3], f[2]);
    eq(z[2], f[3]);

    f = r.find().sort({a: -1, b: 1}).hint(hint);
    eq(z[2], f[0]);
    eq(z[3], f[1]);
    eq(z[0], f[2]);
    eq(z[1], f[3]);

    f = r
        .find({a: {$gte: 2}})
        .sort({a: 1, b: -1})
        .hint(hint);
    eq(z[3], f[0]);
    eq(z[2], f[1]);

    f = r
        .find({a: {$gte: 2}})
        .sort({a: -1, b: 1})
        .hint(hint);
    eq(z[2], f[0]);
    eq(z[3], f[1]);

    f = r
        .find({a: {$gte: 2}})
        .sort({a: 1, b: 1})
        .hint(hint);
    eq(z[2], f[0]);
    eq(z[3], f[1]);

    f = r.find().sort({a: -1, b: -1}).hint(hint);
    eq(z[3], f[0]);
    eq(z[2], f[1]);
    eq(z[1], f[2]);
    eq(z[0], f[3]);
}

let r = db.ed_db_cursor6;
r.drop();

let z = [
    {a: 1, b: 1},
    {a: 1, b: 2},
    {a: 2, b: 1},
    {a: 2, b: 2},
];
for (let i = 0; i < z.length; ++i) r.save(z[i]);

r.createIndex({a: 1, b: -1});

check(false);
check(true);
