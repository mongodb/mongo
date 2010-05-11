t=db.geo_box3;

t.drop();
// SERVER-994
db.foo.insert({ point : { x : -15, y : 10 } });
db.foo.ensureIndex( { point : "2d" } , { min : -21 , max : 21 } );
db.foo.find({point: {"$within": {"$box": [[-20, 7], [0, 15]]} } });
