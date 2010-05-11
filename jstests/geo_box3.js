t=db.geo_box3;

t.drop();
// SERVER-994
t.insert({ point : { x : -15, y : 10 } });
t.ensureIndex( { point : "2d" } , { min : -21 , max : 21 } );
t.find({point: {"$within": {"$box": [[-20, 7], [0, 15]]} } });
