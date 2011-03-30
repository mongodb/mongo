// Make sure nesting of location arrays also works.

t = db.geonest
t.drop();

t.insert( { zip : "10001", data : [ { loc : [ 10, 10 ], type : "home" }, 
									{ loc : [ 50, 50 ], type : "work" } ] } )
t.insert( { zip : "10002", data : [ { loc : [ 20, 20 ], type : "home" }, 
									{ loc : [ 50, 50 ], type : "work" } ] } )
t.insert( { zip : "10003", data : [ { loc : [ 30, 30 ], type : "home" }, 
									{ loc : [ 50, 50 ], type : "work" } ] } )
assert.isnull( db.getLastError() )

t.ensureIndex( { "data.loc" : "2d", zip : 1 } );
assert.isnull( db.getLastError() )
assert.eq( 2, t.getIndexKeys().length )

t.insert( { zip : "10004", data : [ { loc : [ 40, 40 ], type : "home" }, 
									{ loc : [ 50, 50 ], type : "work" } ] } )
assert.isnull( db.getLastError() )

// test normal access

printjson( t.find( { "data.loc" : { $within : { $box : [ [ 0, 0 ], [ 45, 45 ] ] } } } ).toArray() )

assert.eq( 4, t.find( { "data.loc" : { $within : { $box : [ [ 0, 0 ], [ 45, 45 ] ] } } } ).count() );

assert.eq( 4, t.find( { "data.loc" : { $within : { $box : [ [ 45, 45 ], [ 50, 50 ] ] } } } ).count() );





// Try a complex nesting

t = db.geonest
t.drop();

t.insert( { zip : "10001", data : [ { loc : [ [ 10, 10 ], { lat : 50, long : 50 } ], type : "home" } ] } )
t.insert( { zip : "10002", data : [ { loc : [ 20, 20 ], type : "home" }, 
									{ loc : [ 50, 50 ], type : "work" } ] } )
t.insert( { zip : "10003", data : [ { loc : [ { x : 30, y : 30 }, [ 50, 50 ] ], type : "home" } ] } )
assert.isnull( db.getLastError() )

t.ensureIndex( { "data.loc" : "2d", zip : 1 } );
assert.isnull( db.getLastError() )
assert.eq( 2, t.getIndexKeys().length )

t.insert( { zip : "10004", data : [ { loc : [ 40, 40 ], type : "home" }, 
									{ loc : [ 50, 50 ], type : "work" } ] } )

									
assert.isnull( db.getLastError() )

// test normal access
printjson( t.find( { "data.loc" : { $within : { $box : [ [ 0, 0 ], [ 45, 45 ] ] } } } ).toArray() )

assert.eq( 4, t.find( { "data.loc" : { $within : { $box : [ [ 0, 0 ], [ 45, 45 ] ] } } } ).count() );

assert.eq( 4, t.find( { "data.loc" : { $within : { $box : [ [ 45, 45 ], [ 50, 50 ] ] } } } ).count() );



