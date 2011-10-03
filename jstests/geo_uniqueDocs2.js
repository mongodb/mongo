// Additional checks for geo uniqueDocs and includeLocs SERVER-3139.

collName = 'jstests_geo_uniqueDocs2';
t = db[collName];
t.drop();

t.save( {loc:[[20,30],[40,50]]} );
t.ensureIndex( {loc:'2d'} );

// Check exact matches of different locations.
assert.eq( 1, t.count( { loc : [20,30] } ) );
assert.eq( 1, t.count( { loc : [40,50] } ) );

// Check behavior for $near, where $uniqueDocs mode is unavailable.
assert.eq( [t.findOne(),t.findOne()], t.find( { loc: { $near: [50,50] } } ).toArray() );

// Check correct number of matches for $within / $uniqueDocs.
assert.eq( 1, t.count( { loc : { $within : { $center : [[30, 30], 40] } } } ) );
assert.eq( 1, t.count( { loc : { $within : { $center : [[30, 30], 40], $uniqueDocs : true } } } ) );
assert.eq( 2, t.count( { loc : { $within : { $center : [[30, 30], 40], $uniqueDocs : false } } } ) );

// For $within / $uniqueDocs, limit seems to apply to docs, not locs.
assert.eq( 2, t.find( { loc : { $within : { $center : [[30, 30], 40], $uniqueDocs : false } } } ).limit(1).count() );

// Now check a circle only containing one of the locs.
assert.eq( 1, t.count( { loc : { $within : { $center : [[30, 30], 10] } } } ) );
assert.eq( 1, t.count( { loc : { $within : { $center : [[30, 30], 10], $uniqueDocs : true } } } ) );
assert.eq( 1, t.count( { loc : { $within : { $center : [[30, 30], 10], $uniqueDocs : false } } } ) );

// Check number and character of results with geoNear / uniqueDocs / includeLocs.
notUniqueNotInclude = db.runCommand( { geoNear : collName , near : [50,50], num : 10, uniqueDocs : false, includeLocs : false } );
uniqueNotInclude = db.runCommand( { geoNear : collName , near : [50,50], num : 10, uniqueDocs : true, includeLocs : false } );
notUniqueInclude = db.runCommand( { geoNear : collName , near : [50,50], num : 10, uniqueDocs : false, includeLocs : true } );
uniqueInclude = db.runCommand( { geoNear : collName , near : [50,50], num : 10, uniqueDocs : true, includeLocs : true } );

// Check that only unique docs are returned.
assert.eq( 2, notUniqueNotInclude.results.length );
assert.eq( 1, uniqueNotInclude.results.length );
assert.eq( 2, notUniqueInclude.results.length );
assert.eq( 1, uniqueInclude.results.length );

// Check that locs are included.
assert( !notUniqueNotInclude.results[0].loc );
assert( !uniqueNotInclude.results[0].loc );
assert( notUniqueInclude.results[0].loc );
assert( uniqueInclude.results[0].loc );

// For geoNear / uniqueDocs, 'num' limit seems to apply to locs.
assert.eq( 1, db.runCommand( { geoNear : collName , near : [50,50], num : 1, uniqueDocs : false, includeLocs : false } ).results.length );

// Check locs returned in includeLocs mode.
t.remove();
objLocs = [{x:20,y:30,z:['loc1','loca']},{x:40,y:50,z:['loc2','locb']}];
t.save( {loc:objLocs} );
results = db.runCommand( { geoNear : collName , near : [50,50], num : 10, uniqueDocs : false, includeLocs : true } ).results;
assert.contains( results[0].loc, objLocs );
assert.contains( results[1].loc, objLocs );

// Check locs returned in includeLocs mode, where locs are arrays.
t.remove();
arrLocs = [[20,30,['loc1','loca']],[40,50,['loc2','locb']]];
t.save( {loc:arrLocs} );
results = db.runCommand( { geoNear : collName , near : [50,50], num : 10, uniqueDocs : false, includeLocs : true } ).results;
// The original loc arrays are returned as objects.
expectedLocs = arrLocs

assert.contains( results[0].loc, expectedLocs );
assert.contains( results[1].loc, expectedLocs );

// Check matching an additional indexed field inside loc.
t.drop();
t.ensureIndex( {loc:'2d','loc.z':1} );
t.save( {loc:objLocs} );
assert.eq( 1, t.find( { loc : { $within : { $center : [[30, 30], 40], $uniqueDocs : true } }, 'loc.z':'loc2' } ).count() );
assert.eq( 2, t.find( { loc : { $within : { $center : [[30, 30], 40], $uniqueDocs : false } }, 'loc.z':'loc2' } ).count() );
assert.eq( 1, t.find( { loc : { $within : { $center : [[30, 30], 40], $uniqueDocs : true } }, 'loc.z':'loc1' } ).count() );
assert.eq( 2, t.find( { loc : { $within : { $center : [[30, 30], 40], $uniqueDocs : false } }, 'loc.z':'loc1' } ).count() );

// Check matching an additional indexed field inside loc array.
t.drop();
t.ensureIndex( {loc:'2d','loc.1.2':1} );
t.save( {loc:arrLocs} );
assert.eq( 1, t.find( { loc : { $within : { $center : [[30, 30], 40], $uniqueDocs : true } }, 'loc.1.2':'loc2' } ).count() );
assert.eq( 2, t.find( { loc : { $within : { $center : [[30, 30], 40], $uniqueDocs : false } }, 'loc.1.2':'loc2' } ).count() );
assert.eq( 1, t.find( { loc : { $within : { $center : [[30, 30], 40], $uniqueDocs : true } }, 'loc.1.2':'locb' } ).count() );
assert.eq( 2, t.find( { loc : { $within : { $center : [[30, 30], 40], $uniqueDocs : false } }, 'loc.1.2':'locb' } ).count() );

// Test a large number of locations in the array.
t.drop();
arr = [];
for( i = 0; i < 10000; ++i ) {
    arr.push( [10,10] );
}
arr.push( [100,100] );
t.save( {loc:arr} );
t.ensureIndex( {loc:'2d'} );
assert.eq( 1, t.count( { loc : { $within : { $center : [[99, 99], 5] } } } ) );
