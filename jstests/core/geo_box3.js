// How to construct a test to stress the flaw in SERVER-994:
// construct an index, think up a bounding box inside the index that
// doesn't include the center of the index, and put a point inside the
// bounding box.

// This is the bug reported in SERVER-994.
t=db.geo_box3;
t.drop();
t.insert({ point : { x : -15000000, y : 10000000 } });
t.ensureIndex( { point : "2d" } , { min : -21000000 , max : 21000000 } );
var c=t.find({point: {"$within": {"$box": [[-20000000, 7000000], [0, 15000000]]} } });
assert.eq(1, c.count(), "A1");

// Same thing, modulo 1000000.
t=db.geo_box3;
t.drop();
t.insert({ point : { x : -15, y : 10 } });
t.ensureIndex( { point : "2d" } , { min : -21 , max : 21 } );
var c=t.find({point: {"$within": {"$box": [[-20, 7], [0, 15]]} } });
assert.eq(1, c.count(), "B1");

// Two more examples, one where the index is centered at the origin,
// one not.
t=db.geo_box3;
t.drop();
t.insert({ point : { x : 1.0 , y : 1.0 } });
t.ensureIndex( { point : "2d" } , { min : -2 , max : 2 } );
var c=t.find({point: {"$within": {"$box": [[.1, .1], [1.99, 1.99]]} } });
assert.eq(1, c.count(), "C1");

t=db.geo_box3;
t.drop();
t.insert({ point : { x : 3.9 , y : 3.9 } });
t.ensureIndex( { point : "2d" } , { min : 0 , max : 4 } );
var c=t.find({point: {"$within": {"$box": [[2.05, 2.05], [3.99, 3.99]]} } });
assert.eq(1, c.count(), "D1");
