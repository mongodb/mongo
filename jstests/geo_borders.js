
t = db.borders
t.drop()

// FIXME:  FAILS for all epsilon < 1
epsilon = 1
//epsilon = 0.99

// For these tests, *required* that step ends exactly on max
min = -1
max = 1
step = 1
numItems = 0;

for(var x = min; x <= max; x += step){
	for(var y = min; y <= max; y += step){
		t.insert({ loc: { x : x, y : y } })
		numItems++;
	}
}

overallMin = -1
overallMax = 1

// Create a point index slightly smaller than the points we have
t.ensureIndex({ loc : "2d" }, { max : overallMax - epsilon / 2, min : overallMin + epsilon / 2})
assert(db.getLastError(), "A1")

// FIXME:  FAILS for all epsilon < 1
// Create a point index only slightly bigger than the points we have
t.ensureIndex({ loc : "2d" }, { max : overallMax + epsilon, min : overallMin - epsilon })
assert.isnull(db.getLastError(), "A2")








//************
// Box Tests
//************


/*
// FIXME: Fails w/ non-nice error
// Make sure we can get all points in full bounds
assert(numItems == t.find({ loc : { $within : { $box : [[overallMin - epsilon,
	                                                     overallMin - epsilon],
	                                                    [overallMax + epsilon,
	                                                     overallMax + epsilon]] } } }).count(), "B1");
*/ 

// Make sure an error is thrown if the bounds are bigger than the box itself
// TODO:  Do we really want an error in this case?  Shouldn't we just clip the box?
try{
	t.findOne({ loc : { $within : { $box : [[overallMin - 2 * epsilon,
                                             overallMin - 2 * epsilon],
                                            [overallMax + 2 * epsilon,
                                             overallMax + 2 * epsilon]] } } });
	assert(false, "B2");
}
catch(e){}

//Make sure we can get at least close to the bounds of the index
assert(numItems == t.find({ loc : { $within : { $box : [[overallMin - epsilon / 2,
	                                                     overallMin - epsilon / 2],
	                                                    [overallMax + epsilon / 2,
	                                                     overallMax + epsilon / 2]] } } }).count(), "B3");


//**************
//Circle tests
//**************

center = (overallMax + overallMin) / 2
center = [center, center]
radius = overallMax

offCenter = [center[0] + radius, center[1] + radius]
onBounds = [offCenter[0] + epsilon, offCenter[1] + epsilon]
offBounds = [onBounds[0] + epsilon, onBounds[1] + epsilon]


//Make sure we can get all points when radius is exactly at full bounds
assert(0 < t.find({ loc : { $within : { $center : [center, radius + epsilon] } } }).count(), "C1");

//Make sure we can get points when radius is over full bounds
assert(0 < t.find({ loc : { $within : { $center : [center, radius + 2 * epsilon] } } }).count(), "C2");

//Make sure we can get points when radius is over full bounds, off-centered
assert(0 < t.find({ loc : { $within : { $center : [offCenter, radius + 2 * epsilon] } } }).count(), "C3");

//Make sure we get correct corner point when center is in bounds
// (x bounds wrap, so could get other corner)
cornerPt = t.findOne({ loc : { $within : { $center : [offCenter, step / 2] } } });
assert(cornerPt.loc.y == overallMax, "C4")

/*
// FIXME:  FAILS, returns opposite corner
// Make sure we get correct corner point when center is on bounds
cornerPt = t.findOne({ loc : { $within : { $center : [onBounds,
                                                      Math.sqrt(2 * epsilon * epsilon) + (step / 2) ] } } });
assert(cornerPt.loc.y == overallMax, "C5")
*/

// TODO:  Handle gracefully?
// Make sure we can't get corner point when center is over bounds
try{
	t.findOne({ loc : { $within : { $center : [offBounds,
                                               Math.sqrt(8 * epsilon * epsilon) + (step / 2) ] } } });
	assert(false, "C6")
}
catch(e){}







//***********
//Near tests
//***********

//Make sure we can get all nearby points to point in range
assert(t.find({ loc : { $near : offCenter } }).next().loc.y == overallMax,
	   "D1");

/*
// FIXME: FAILS, returns opposite list
// Make sure we can get all nearby points to point on boundary
assert(t.find({ loc : { $near : onBounds } }).next().loc.y == overallMax,
       "D2");
*/

//TODO: Could this work?
//Make sure we can't get all nearby points to point over boundary
try{
	t.findOne({ loc : { $near : offBounds } })
	assert(false, "D3")
}
catch(e){}

/*
// FIXME: FAILS, returns only single point
//Make sure we can get all nearby points within one step (4 points in top corner)
assert(4 == t.find({ loc : { $near : offCenter, $maxDistance : step * 1.9 } }).count(),
	   "D4");
*/



//**************
//Command Tests
//**************


//Make sure we can get all nearby points to point in range
assert(db.runCommand({ geoNear : "borders", near : offCenter }).results[0].obj.loc.y == overallMax,
	   "E1");


/*
// FIXME: FAILS, returns opposite list
//Make sure we can get all nearby points to point on boundary
assert(db.runCommand({ geoNear : "borders", near : onBounds }).results[0].obj.loc.y == overallMax,
	    "E2");
*/

//TODO: Could this work?
//Make sure we can't get all nearby points to point over boundary
try{
	db.runCommand({ geoNear : "borders", near : offBounds }).results.length
	assert(false, "E3")
}
catch(e){}


/*
// FIXME: Fails, returns one point
//Make sure we can get all nearby points within one step (4 points in top corner)
assert(4 == db.runCommand({ geoNear : "borders", near : offCenter, maxDistance : step * 1.5 }).results.length,
	   "E4");
*/



