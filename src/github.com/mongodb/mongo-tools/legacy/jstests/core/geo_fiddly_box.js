// Reproduces simple test for SERVER-2832

// The setup to reproduce was/is to create a set of points where the 
// "expand" portion of the geo-lookup expands the 2d range in only one
// direction (so points are required on either side of the expanding range)

t = db.geo_fiddly_box

t.drop();
t.ensureIndex({ loc : "2d" })

t.insert({ "loc" : [3, 1] })
t.insert({ "loc" : [3, 0.5] })
t.insert({ "loc" : [3, 0.25] })
t.insert({ "loc" : [3, -0.01] })
t.insert({ "loc" : [3, -0.25] })
t.insert({ "loc" : [3, -0.5] })
t.insert({ "loc" : [3, -1] })

// OK!
print( t.count() )
assert.eq( 7, t.count({ "loc" : { "$within" : { "$box" : [ [2, -2], [46, 2] ] } } }), "Not all locations found!" );


// Test normal lookup of a small square of points as a sanity check.

epsilon = 0.0001;
min = -1
max = 1
step = 1
numItems = 0;

t.drop()
t.ensureIndex({ loc : "2d" }, { max : max + epsilon / 2, min : min - epsilon / 2 })

for(var x = min; x <= max; x += step){
	for(var y = min; y <= max; y += step){
		t.insert({ "loc" : { x : x, y : y } })
		numItems++;
	}
}

assert.eq( numItems, t.count({ loc : { $within : { $box : [[min - epsilon / 3,
                                                                    min - epsilon / 3],
                                                                   [max + epsilon / 3,
                                                                    max + epsilon / 3]] } } }), "Not all locations found!");
