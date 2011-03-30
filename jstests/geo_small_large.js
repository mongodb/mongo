// SERVER-2386, general geo-indexing using very large and very small bounds

load( "jstests/libs/geo_near_random.js" );

// Do some random tests (for near queries) with very large and small ranges

var test = new GeoNearRandomTest( "geo_small_large" );

bounds = { min : -Math.pow( 2, 34 ), max : Math.pow( 2, 34 ) };

test.insertPts( 50, bounds );

printjson( db["geo_small_large"].find().limit( 10 ).toArray() )

test.testPt( [ 0, 0 ] );
test.testPt( test.mkPt( undefined, bounds ) );
test.testPt( test.mkPt( undefined, bounds ) );
test.testPt( test.mkPt( undefined, bounds ) );
test.testPt( test.mkPt( undefined, bounds ) );

test = new GeoNearRandomTest( "geo_small_large" );

bounds = { min : -Math.pow( 2, -34 ), max : Math.pow( 2, -34 ) };

test.insertPts( 50, bounds );

printjson( db["geo_small_large"].find().limit( 10 ).toArray() )

test.testPt( [ 0, 0 ] );
test.testPt( test.mkPt( undefined, bounds ) );
test.testPt( test.mkPt( undefined, bounds ) );
test.testPt( test.mkPt( undefined, bounds ) );
test.testPt( test.mkPt( undefined, bounds ) );


// Check that our box and circle queries also work
var scales = [ Math.pow( 2, 40 ), Math.pow( 2, -40 ), Math.pow(2, 2), Math.pow(3, -15), Math.pow(3, 15) ]

for ( var i = 0; i < scales.length; i++ ) {

	scale = scales[i];

	var eps = Math.pow( 2, -7 ) * scale;
	var radius = 5 * scale;
	var max = 10 * scale;
	var min = -max;
	var range = max - min;

	var t = db["geo_small_large"]
	t.drop();
	t.ensureIndex( { p : "2d" }, { min : min, max : max, bits : 20 + Math.random() * 10 })

	var outPoints = 0;
	var inPoints = 0;
	
	// Put a point slightly inside and outside our range
	for ( var j = 0; j < 2; j++ ) {
		var currRad = ( j % 2 == 0 ? radius + eps : radius - eps );
		t.insert( { p : { x : currRad, y : 0 } } );
		print( db.getLastError() )
	}
	
	printjson( t.find().toArray() );

	assert.eq( t.count( { p : { $within : { $center : [[0, 0], radius ] } } } ), 1, "Incorrect center points found!" )
	assert.eq( t.count( { p : { $within : { $box : [ [ -radius, -radius ], [ radius, radius ] ] } } } ), 1,
			"Incorrect box points found!" )

	for ( var j = 0; j < 30; j++ ) {

		var x = Math.random() * ( range - eps ) + eps + min;
		var y = Math.random() * ( range - eps ) + eps + min;

		t.insert( { p : [ x, y ] } );

		if ( x * x + y * y > radius * radius )
			outPoints++
		else
			inPoints++
	}

	assert.eq( t.count( { p : { $within : { $center : [[0, 0], radius ] } } } ), 1 + inPoints,
			"Incorrect random center points found!" )
			
	print("Found " + inPoints + " points in and " + outPoints + " points out.");

}

