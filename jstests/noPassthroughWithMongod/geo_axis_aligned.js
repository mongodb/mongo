// Axis aligned circles - hard-to-find precision errors possible with exact distances here

t = db.axisaligned
t.drop();

scale = [ 1, 10, 1000, 10000 ]
bits = [ 2, 3, 4, 5, 6, 7, 8, 9 ]
radius = [ 0.0001, 0.001, 0.01, 0.1 ]
center = [ [ 5, 52 ], [ 6, 53 ], [ 7, 54 ], [ 8, 55 ], [ 9, 56 ] ]

bound = []
for( var j = 0; j < center.length; j++ ) bound.push( [-180, 180] );

// Scale all our values to test different sizes
radii = []
centers = []
bounds = []

for( var s = 0; s < scale.length; s++ ){
	for ( var i = 0; i < radius.length; i++ ) {
		radii.push( radius[i] * scale[s] )
	}
	
	for ( var j = 0; j < center.length; j++ ) {
		centers.push( [ center[j][0] * scale[s], center[j][1] * scale[s] ] )
		bounds.push( [ bound[j][0] * scale[s], bound[j][1] * scale[s] ] )
	}	

}

radius = radii
center = centers
bound = bounds


for ( var b = 0; b < bits.length; b++ ) {
	
	
	printjson( radius )
	printjson( centers )
	
	for ( var i = 0; i < radius.length; i++ ) {
		for ( var j = 0; j < center.length; j++ ) {
			
			printjson( { center : center[j], radius : radius[i], bits : bits[b] } );

			t.drop()
			
			// Make sure our numbers are precise enough for this test
			if( (center[j][0] - radius[i] == center[j][0]) || (center[j][1] - radius[i] == center[j][1]) )
				continue;
			
			t.save( { "_id" : 1, "loc" : { "x" : center[j][0] - radius[i], "y" : center[j][1] } } );
			t.save( { "_id" : 2, "loc" : { "x" : center[j][0], "y" : center[j][1] } } );
			t.save( { "_id" : 3, "loc" : { "x" : center[j][0] + radius[i], "y" : center[j][1] } } );
			t.save( { "_id" : 4, "loc" : { "x" : center[j][0], "y" : center[j][1] + radius[i] } } );
			t.save( { "_id" : 5, "loc" : { "x" : center[j][0], "y" : center[j][1] - radius[i] } } );
			t.save( { "_id" : 6, "loc" : { "x" : center[j][0] - radius[i], "y" : center[j][1] + radius[i] } } );
			t.save( { "_id" : 7, "loc" : { "x" : center[j][0] + radius[i], "y" : center[j][1] + radius[i] } } );
			t.save( { "_id" : 8, "loc" : { "x" : center[j][0] - radius[i], "y" : center[j][1] - radius[i] } } );
			t.save( { "_id" : 9, "loc" : { "x" : center[j][0] + radius[i], "y" : center[j][1] - radius[i] } } );

			t.ensureIndex( { loc : "2d" }, { max : bound[j][1], min : bound[j][0], bits : bits[b] } );
			
			if( db.getLastError() ) continue;
			
			print( "DOING WITHIN QUERY ")
			r = t.find( { "loc" : { "$within" : { "$center" : [ center[j], radius[i] ] } } } );
			
			//printjson( r.toArray() );
			
			assert.eq( 5, r.count() );

			// FIXME: surely code like this belongs in utils.js.
			a = r.toArray();
			x = [];
			for ( k in a )
				x.push( a[k]["_id"] )
			x.sort()
			assert.eq( [ 1, 2, 3, 4, 5 ], x );

			print( " DOING NEAR QUERY ")
			//printjson( center[j] )
			r = t.find( { loc : { $near : center[j], $maxDistance : radius[i] } }, { _id : 1 } )
			assert.eq( 5, r.count() );
			
			print( " DOING DIST QUERY ")
			
			a = db.runCommand({ geoNear : "axisaligned", near : center[j], maxDistance : radius[i] }).results
			assert.eq( 5, a.length );
			
			//printjson( a );
			
			var distance = 0;
			for( var k = 0; k < a.length; k++ ){
				//print( a[k].dis )
				//print( distance )
				assert.gte( a[k].dis, distance );
				//printjson( a[k].obj )
				//print( distance = a[k].dis );
			}
			
			r = t.find( { loc : { $within : { $box : [ [ center[j][0] - radius[i], center[j][1] - radius[i] ], [ center[j][0] + radius[i], center[j][1] + radius[i] ] ] } } }, { _id : 1 } )
			assert.eq( 9, r.count() );

		}
	}
}