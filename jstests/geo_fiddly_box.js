// Reproduces simple test for SERVER-2832

// The setup to reproduce was/is to create a set of points where the 
// "expand" portion of the geo-lookup expands the 2d range in only one
// direction (so points are required on either side of the expanding range)

db.geo_fiddly_box.drop();
db.geo_fiddly_box.ensureIndex({ loc : "2d" })

db.geo_fiddly_box.insert({ "loc" : [3, 1] })
db.geo_fiddly_box.insert({ "loc" : [3, 0.5] })
db.geo_fiddly_box.insert({ "loc" : [3, 0.25] })
db.geo_fiddly_box.insert({ "loc" : [3, -0.01] })
db.geo_fiddly_box.insert({ "loc" : [3, -0.25] })
db.geo_fiddly_box.insert({ "loc" : [3, -0.5] })
db.geo_fiddly_box.insert({ "loc" : [3, -1] })

// OK!
print( db.geo_fiddly_box.count() )
assert.eq( 7, db.geo_fiddly_box.count({ "loc" : { "$within" : { "$box" : [ [2, -2], [46, 2] ] } } }), "Not all locations found!" );


// Test normal lookup of a small square of points as a sanity check.

epsilon = 0.0001;
min = -1
max = 1
step = 1
numItems = 0;

db.geo_fiddly_box2.drop()
db.geo_fiddly_box2.ensureIndex({ loc : "2d" }, { max : max + epsilon / 2, min : min - epsilon / 2 })

for(var x = min; x <= max; x += step){
	for(var y = min; y <= max; y += step){
		db.geo_fiddly_box2.insert({ "loc" : { x : x, y : y } })
		numItems++;
	}
}

assert.eq( numItems, db.geo_fiddly_box2.count({ loc : { $within : { $box : [[min - epsilon / 3,
                                                                    min - epsilon / 3],
                                                                   [max + epsilon / 3,
                                                                    max + epsilon / 3]] } } }), "Not all locations found!");
