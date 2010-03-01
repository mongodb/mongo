
t = db.geo1
t.drop();

idx = { loc : "2d" , zip : 1 }

t.insert( { zip : "06525" , loc : [ 41.352964 , 73.01212  ] } )
t.insert( { zip : "10024" , loc : [ 40.786387 , 73.97709 ] } )
t.insert( { zip : "94061" , loc : [ 37.463911 , 122.23396 ] } )
assert.isnull( db.getLastError() )

// test "2d" has to be first
assert.eq( 1 , t.getIndexKeys().length , "S1" );
t.ensureIndex( { zip : 1 , loc : "2d" } );
assert.eq( 1 , t.getIndexKeys().length , "S2" );

t.ensureIndex( idx );
assert.eq( 2 , t.getIndexKeys().length , "S3" );

assert.eq( 3 , t.count() , "B1" );
t.insert( { loc : [ 200 , 200 ]  } )
assert( db.getLastError() , "B2" )
assert.eq( 3 , t.count() , "B3" );

// test normal access

wb = t.findOne( { zip : "06525" } )
assert( wb , "C1" );

assert.eq( "06525" , t.find( { loc : wb.loc } ).hint( { "$natural" : 1 } )[0].zip , "C2" )
assert.eq( "06525" , t.find( { loc : wb.loc } )[0].zip , "C3" )
assert.eq( 1 , t.find( { loc : wb.loc } ).explain().nscanned , "C4" )

// test config options

t.drop();

t.ensureIndex( { loc : "2d" } , { min : -500 , max : 500 , bits : 4 } );
t.insert( { loc : [ 200 , 200 ]  } )
assert.isnull( db.getLastError() , "D1" )

