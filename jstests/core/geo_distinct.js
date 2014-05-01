// Tests distinct with geospatial field values.
// 1. Test distinct with geo values for 'key' (SERVER-2135)

var coll = db.geo_distinct;
var res;

//
// 1. Test distinct with geo values for 'key'.
//

coll.drop();
coll.insert( { loc: { type: 'Point', coordinates: [ 10, 20 ] } } );
coll.insert( { loc: { type: 'Point', coordinates: [ 10, 20 ] } } );
coll.insert( { loc: { type: 'Point', coordinates: [ 20, 30 ] } } );
coll.insert( { loc: { type: 'Point', coordinates: [ 20, 30 ] } } );
assert.eq( 4, coll.count() );

// Test distinct on GeoJSON points with/without a 2dsphere index.

res = coll.runCommand( 'distinct', { key: 'loc' } );
assert.commandWorked( res );
assert.eq( res.values.sort(), [ { type: 'Point', coordinates: [ 10, 20 ] },
                                { type: 'Point', coordinates: [ 20, 30 ] } ] );

assert.commandWorked( coll.ensureIndex( { loc: '2dsphere' } ) );

res = coll.runCommand( 'distinct', { key: 'loc' } );
assert.commandWorked( res );
assert.eq( res.values.sort(), [ { type: 'Point', coordinates: [ 10, 20 ] },
                                { type: 'Point', coordinates: [ 20, 30 ] } ] );

// Test distinct on legacy points with/without a 2d index.

// (Note that distinct on a 2d-indexed field doesn't produce a list of coordinate pairs, since
// distinct logically operates on unique values in an array.  Hence, the results are unintuitive and
// not semantically meaningful.)

coll.dropIndexes();

res = coll.runCommand( 'distinct', { key: 'loc.coordinates' } );
assert.commandWorked( res );
assert.eq( res.values.sort(), [ 10, 20, 30 ] );

assert.commandWorked( coll.ensureIndex( { 'loc.coordinates': '2d' } ) );

res = coll.runCommand( 'distinct', { key: 'loc.coordinates' } );
assert.commandWorked( res );
assert.eq( res.values.sort(), [ 10, 20, 30 ] );
