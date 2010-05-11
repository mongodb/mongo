// How to construct a test to stress the flaw in SERVER-994:
// construct an index, think up a bounding box inside the index that
// doesn't include the center of the index, and put a point inside the
// bounding box.

// This is the bug reported in SERVER-994, modulo 1000000.
t=db.geo_box3;
t.drop();
t.insert({ point : { x : -15, y : 10 } });
t.ensureIndex( { point : "2d" } , { min : -21 , max : 21 } );
t.find({point: {"$within": {"$box": [[-20, 7], [0, 15]]} } });

// Two more examples, one where the index is centered at the origin,
// one not.
t=db.geo_box3;
t.drop();
t.insert({ point : { x : 1.0 , y : 1.0 } });
t.ensureIndex( { point : "2d" } , { min : -2 , max : 2 } );
t.find({point: {"$within": {"$box": [[.1, .1], [1.99, 1.99]]} } });

t=db.geo_box3;
t.drop();
t.insert({ point : { x : 3.9 , y : 3.9 } });
t.ensureIndex( { point : "2d" } , { min : 0 , max : 4 } );
t.find({point: {"$within": {"$box": [[2.05, 2.05], [3.99, 3.99]]} } });
