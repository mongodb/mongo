// Reproduces simple test for SERVER-2832

// The setup to reproduce was/is to create a set of points where the 
// "expand" portion of the geo-lookup expands the 2d range in only one
// direction (so points are required on either side of the expanding range)

db.users.drop();
db.users.ensureIndex({ loc : "2d" })

db.users.insert({ "loc" : [3, 1] })
db.users.insert({ "loc" : [3, 0.5] })
db.users.insert({ "loc" : [3, 0.25] })
db.users.insert({ "loc" : [3, -0.01] })
db.users.insert({ "loc" : [3, -0.25] })
db.users.insert({ "loc" : [3, -0.5] })
db.users.insert({ "loc" : [3, -1] })

// OK!
print( db.users.count() )
assert.eq( 7, db.users.count({ "loc" : { "$within" : { "$box" : [ [2, -2], [46, 2] ] } } }), "Not all locations found!" );


// Test normal lookup of a small square of points as a sanity check.

epsilon = 0.0001;
min = -1
max = 1
step = 1
numItems = 0;

db.users2.drop()
db.users2.ensureIndex({ loc : "2d" }, { max : max + epsilon / 2, min : min - epsilon / 2 })

for(var x = min; x <= max; x += step){
	for(var y = min; y <= max; y += step){
		db.users2.insert({ "loc" : { x : x, y : y } })
		numItems++;
	}
}

assert.eq( numItems, db.users2.count({ loc : { $within : { $box : [[min - epsilon / 3,
                                                                    min - epsilon / 3],
                                                                   [max + epsilon / 3,
                                                                    max + epsilon / 3]] } } }), "Not all locations found!");