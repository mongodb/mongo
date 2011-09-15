// Test that weird polygons work SERVER-3725

t = db.geo_polygon5;
t.drop();

t.insert({loc:[0,0]})
t.insert({loc:[1,0]})
t.insert({loc:[2,0]})
t.insert({loc:[3,0]})
t.insert({loc:[4,0]})

t.ensureIndex( { loc : "2d" } );

printjson( t.find({ loc: { "$within": { "$polygon" : [[0,0], [2,0], [4,0]] } } }).toArray() )

assert.eq( 5, t.find({ loc: { "$within": { "$polygon" : [[0,0], [2,0], [4,0]] } } }).itcount() )

