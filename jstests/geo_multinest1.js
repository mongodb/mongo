// Test distance queries with interleaved distances

t = db.multinest
t.drop();

t.insert( { zip : "10001", data : [ { loc : [ 10, 10 ], type : "home" }, 
									{ loc : [ 29, 29 ], type : "work" } ] } )
t.insert( { zip : "10002", data : [ { loc : [ 20, 20 ], type : "home" }, 
									{ loc : [ 39, 39 ], type : "work" } ] } )
t.insert( { zip : "10003", data : [ { loc : [ 30, 30 ], type : "home" }, 
									{ loc : [ 49, 49 ], type : "work" } ] } )
assert.isnull( db.getLastError() )

t.ensureIndex( { "data.loc" : "2d", zip : 1 } );
assert.isnull( db.getLastError() )
assert.eq( 2, t.getIndexKeys().length )

t.insert( { zip : "10004", data : [ { loc : [ 40, 40 ], type : "home" }, 
									{ loc : [ 59, 59 ], type : "work" } ] } )
assert.isnull( db.getLastError() )

// test normal access

var result = t.find({ "data.loc" : { $near : [0, 0] } }).toArray();

printjson( result )

assert.eq( 8, result.length )

var order = [ 1, 2, 1, 3, 2, 4, 3, 4 ]

for( var i = 0; i < result.length; i++ ){
	assert.eq( "1000" + order[i], result[i].zip )
}



