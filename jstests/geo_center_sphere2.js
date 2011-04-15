//
// Tests the error handling of spherical queries
//

var numTests = 30

for ( var test = 0; test < numTests; test++ ) {

	Random.srand( 1337 + test );

	var radius = 5000 * Random.rand() // km
	radius = radius / 6371 // radians
	var numPoints = Math.floor( 3000 * Random.rand() )
	// TODO: Wrapping uses the error value to figure out what would overlap...
	var bits = Math.floor( 5 + Random.rand() * 28 )

	t = db.sphere

	var randomPoint = function() {
		return [ Random.rand() * 360 - 180, Random.rand() * 180 - 90 ];
	}

	var pointsIn = 0
	var pointsOut = 0

	// Get a start point that doesn't require wrapping
	// TODO: Are we a bit too aggressive with wrapping issues?
	var startPoint
	var ex = null
	do {

		t.drop()
		startPoint = randomPoint()
		t.ensureIndex( { loc : "2d" }, { bits : bits } )

		try {
			// Check for wrapping issues
			t.find( { loc : { $within : { $centerSphere : [ startPoint, radius ] } } } ).toArray()
			ex = null
		} catch (e) {
			ex = e
		}
	} while (ex)

	for ( var i = 0; i < numPoints; i++ ) {

		var point = randomPoint()

		t.insert( { loc : point } )

		if ( Geo.sphereDistance( startPoint, point ) <= radius )
			pointsIn++;
		else
			pointsOut++;

	}

	assert.isnull( db.getLastError() )

	printjson( { radius : radius, numPoints : numPoints, pointsIn : pointsIn, pointsOut : pointsOut } )
	
	// $centerSphere
	assert.eq( pointsIn , t.find( { loc : { $within : { $centerSphere : [ startPoint, radius ] } } } ).count() )
	
	// $nearSphere
	var results = t.find( { loc : { $nearSphere : startPoint, $maxDistance : radius } } ).limit(2 * pointsIn).toArray()
	assert.eq( pointsIn , results.length )
	
	var distance = 0;
	for( var i = 0; i < results.length; i++ ){
		var newDistance = Geo.sphereDistance( startPoint, results[i].loc )
		// print( "Dist from : " + results[i].loc + " to " + startPoint + " is " + newDistance + " vs " + radius )
		assert.lte( newDistance, radius )
		assert.gte( newDistance, distance )
		distance = newDistance
	}
	
	// geoNear
	var results = db.runCommand({ geoNear : "sphere", near : startPoint, maxDistance : radius, num : 2 * pointsIn, spherical : true }).results
	assert.eq( pointsIn , results.length )
	
	var distance = 0;
	for( var i = 0; i < results.length; i++ ){
		var retDistance = results[i].dis
		var newDistance = Geo.sphereDistance( startPoint, results[i].obj.loc )
		// print( "Dist from : " + results[i].loc + " to " + startPoint + " is " + newDistance + " vs " + radius )
		assert( newDistance >= retDistance - 0.0001 && newDistance <= retDistance + 0.0001 )
		assert.lte( retDistance, radius )
		assert.gte( retDistance, distance )
		assert.lte( newDistance, radius )
		assert.gte( newDistance, distance )
		distance = retDistance
	}
	
	
	
}
