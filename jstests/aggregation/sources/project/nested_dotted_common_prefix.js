// SERVER-6184 Support mixing nested and dotted fields with common prefixes
// @tags: [tests_projection_field_order]

let c = db.c;
c.drop();

c.save({a: "missing", b: {c: "bar", a: "baz", z: "not there"}});

function test(projection) {
    let res = c.aggregate({$project: projection});
    assert.eq(res.toArray()[0], {b: {c: "bar", a: "baz"}});
}

test({_id: 0, b: {a: 1}, "b.c": 1});
test({_id: 0, "b.c": 1, b: {a: 1}});

// Synthetic fields should be in the order they appear in the $project

let one = {$add: [1]};
let res = c.aggregate({$project: {_id: 0, "A.Z": one, A: {Y: one, A: one}, "A.B": one}});
assert.eq(res.toArray()[0], {A: {Z: 1, Y: 1, A: 1, B: 1}});
