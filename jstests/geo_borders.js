t = db.borders
t.drop()

epsilon = 0.0001;

// For these tests, *required* that step ends exactly on max
min = -1
max = 1
step = 1
numItems = 0;

for ( var x = min; x <= max; x += step ) {
	for ( var y = min; y <= max; y += step ) {
		t.insert( { loc : { x : x, y : y } } )
		numItems++;
	}
}

overallMin = -1
overallMax = 1

// Create a point index slightly smaller than the points we have
t.ensureIndex( { loc : "2d" }, { max : overallMax - epsilon / 2, min : overallMin + epsilon / 2 } )
assert( db.getLastError() )

// Create a point index only slightly bigger than the points we have
t.ensureIndex( { loc : "2d" }, { max : overallMax + epsilon, min : overallMin - epsilon } )
assert.isnull( db.getLastError() )

// ************
// Box Tests
// ************

// If the bounds are bigger than the box itself, just clip at the borders
assert.eq( numItems, t.find(
		{ loc : { $within : { $box : [
				[ overallMin - 2 * epsilon, overallMin - 2 * epsilon ],
				[ overallMax + 2 * epsilon, overallMax + 2 * epsilon ] ] } } } ).count() );

// Check this works also for bounds where only a single dimension is off-bounds
assert.eq( numItems - 5, t.find(
		{ loc : { $within : { $box : [
				[ overallMin - 2 * epsilon, overallMin - 0.5 * epsilon ],
				[ overallMax - epsilon, overallMax - epsilon ] ] } } } ).count() );

// Make sure we can get at least close to the bounds of the index
assert.eq( numItems, t.find(
		{ loc : { $within : { $box : [
				[ overallMin - epsilon / 2, overallMin - epsilon / 2 ],
				[ overallMax + epsilon / 2, overallMax + epsilon / 2 ] ] } } } ).count() );

// Make sure we can get at least close to the bounds of the index
assert.eq( numItems, t.find(
		{ loc : { $within : { $box : [
				[ overallMax + epsilon / 2, overallMax + epsilon / 2 ],
				[ overallMin - epsilon / 2, overallMin - epsilon / 2 ] ] } } } ).count() );

// Check that swapping min/max has good behavior
assert.eq( numItems, t.find(
		{ loc : { $within : { $box : [
				[ overallMax + epsilon / 2, overallMax + epsilon / 2 ],
				[ overallMin - epsilon / 2, overallMin - epsilon / 2 ] ] } } } ).count() );

assert.eq( numItems, t.find(
		{ loc : { $within : { $box : [
				[ overallMax + epsilon / 2, overallMin - epsilon / 2 ],
				[ overallMin - epsilon / 2, overallMax + epsilon / 2 ] ] } } } ).count() );

// **************
// Circle tests
// **************

center = ( overallMax + overallMin ) / 2
center = [ center, center ]
radius = overallMax

offCenter = [ center[0] + radius, center[1] + radius ]
onBounds = [ offCenter[0] + epsilon, offCenter[1] + epsilon ]
offBounds = [ onBounds[0] + epsilon, onBounds[1] + epsilon ]
onBoundsNeg = [ -onBounds[0], -onBounds[1] ]

// Make sure we can get all points when radius is exactly at full bounds
assert.lt( 0, t.find( { loc : { $within : { $center : [ center, radius + epsilon ] } } } ).count() );

// Make sure we can get points when radius is over full bounds
assert.lt( 0, t.find( { loc : { $within : { $center : [ center, radius + 2 * epsilon ] } } } ).count() );

// Make sure we can get points when radius is over full bounds, off-centered
assert.lt( 0, t.find( { loc : { $within : { $center : [ offCenter, radius + 2 * epsilon ] } } } ).count() );

// Make sure we get correct corner point when center is in bounds
// (x bounds wrap, so could get other corner)
cornerPt = t.findOne( { loc : { $within : { $center : [ offCenter, step / 2 ] } } } );
assert.eq( cornerPt.loc.y, overallMax )

// Make sure we get correct corner point when center is on bounds
// NOTE: Only valid points on MIN bounds
cornerPt = t
		.findOne( { loc : { $within : { $center : [ onBoundsNeg, Math.sqrt( 2 * epsilon * epsilon ) + ( step / 2 ) ] } } } );
assert.eq( cornerPt.loc.y, overallMin )

// Make sure we can't get corner point when center is over bounds
try {
	t.findOne( { loc : { $within : { $center : [ offBounds, Math.sqrt( 8 * epsilon * epsilon ) + ( step / 2 ) ] } } } );
	assert( false )
} catch (e) {
}

// Make sure we can't get corner point when center is on max bounds
try {
	t.findOne( { loc : { $within : { $center : [ onBounds, Math.sqrt( 8 * epsilon * epsilon ) + ( step / 2 ) ] } } } );
	assert( false )
} catch (e) {
}

// ***********
// Near tests
// ***********

// Make sure we can get all nearby points to point in range
assert.eq( overallMax, t.find( { loc : { $near : offCenter } } ).next().loc.y );

// Make sure we can get all nearby points to point on boundary
assert.eq( overallMin, t.find( { loc : { $near : onBoundsNeg } } ).next().loc.y );

// Make sure we can't get all nearby points to point over boundary
try {
	t.findOne( { loc : { $near : offBounds } } )
	assert( false )
} catch (e) {
}
// Make sure we can't get all nearby points to point on max boundary
try {
	t.findOne( { loc : { $near : onBoundsNeg } } )
	assert( false )
} catch (e) {
}

// Make sure we can get all nearby points within one step (4 points in top
// corner)
assert.eq( 4, t.find( { loc : { $near : offCenter, $maxDistance : step * 1.9 } } ).count() );

// **************
// Command Tests
// **************
// Make sure we can get all nearby points to point in range
assert.eq( overallMax, db.runCommand( { geoNear : "borders", near : offCenter } ).results[0].obj.loc.y );

// Make sure we can get all nearby points to point on boundary
assert.eq( overallMin, db.runCommand( { geoNear : "borders", near : onBoundsNeg } ).results[0].obj.loc.y );

// Make sure we can't get all nearby points to point over boundary
try {
	db.runCommand( { geoNear : "borders", near : offBounds } ).results.length
	assert( false )
} catch (e) {
}

// Make sure we can't get all nearby points to point on max boundary
try {
	db.runCommand( { geoNear : "borders", near : onBounds } ).results.length
	assert( false )
} catch (e) {
}

// Make sure we can get all nearby points within one step (4 points in top
// corner)
assert.eq( 4, db.runCommand( { geoNear : "borders", near : offCenter, maxDistance : step * 1.5 } ).results.length );
