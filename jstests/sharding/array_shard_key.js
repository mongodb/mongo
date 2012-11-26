// Ensure you can't shard on an array key

var st = new ShardingTest({ name : jsTestName(), shards : 3 })

var mongos = st.s0

var coll = mongos.getCollection( jsTestName() + ".foo" )

st.shardColl( coll, { _id : 1, i : 1 }, { _id : ObjectId(), i : 1 } )

printjson( mongos.getDB("config").chunks.find().toArray() )

st.printShardingStatus()

print( "1: insert some invalid data" )

var value = null

var checkError = function( shouldError ){
    var error = coll.getDB().getLastError()
    
    if( error != null ) printjson( error )
    
    if( error == null && ! shouldError ) return
    if( error != null && shouldError ) return
    
    if( error == null ) print( "No error detected!" )
    else print( "Unexpected error!" )
    
    assert( false )
}

// Insert an object with invalid array key 
coll.insert({ i : [ 1, 2 ] })
checkError( true )

// Insert an object with all the right fields, but an invalid array val for _id
coll.insert({ _id : [ 1, 2 ] , i : 3})
checkError( true )

// Insert an object with valid array key
coll.insert({ i : 1 })
checkError( false )

// Update the value with valid other field
value = coll.findOne({ i : 1 })
coll.update( value, { $set : { j : 2 } } )
checkError( false )

// Update the value with invalid other fields
value = coll.findOne({ i : 1 })
coll.update( value, Object.merge( value, { i : [ 3 ] } ) )
checkError( true )

// Multi-update the value with invalid other fields
value = coll.findOne({ i : 1 })
coll.update( value, Object.merge( value, { i : [ 3, 4 ] } ), false, true)
checkError( true )

// Single update the value with valid other fields
value = coll.findOne({ i : 1 })
coll.update( Object.merge( value, { i : [ 3, 4 ] } ), value )
checkError( true )

// Multi-update the value with other fields (won't work, but no error)
value = coll.findOne({ i : 1 })
coll.update( Object.merge( value, { i : [ 1, 1 ] } ), { $set : { k : 4 } }, false, true)
checkError( false )

// Query the value with other fields (won't work, but no error)
value = coll.findOne({ i : 1 })
coll.find( Object.merge( value, { i : [ 1, 1 ] } ) ).toArray()
checkError( false )

// Can't remove using multikey, but shouldn't error
value = coll.findOne({ i : 1 })
coll.remove( Object.extend( value, { i : [ 1, 2, 3, 4 ] } ) )
checkError( false )

// Can't remove using multikey, but shouldn't error
value = coll.findOne({ i : 1 })
coll.remove( Object.extend( value, { i : [ 1, 2, 3, 4, 5 ] } ) )
error = coll.getDB().getLastError()
assert.eq( error, null )
assert.eq( coll.find().itcount(), 1 )

value = coll.findOne({ i : 1 })
coll.remove( Object.extend( value, { i : 1 } ) )
error = coll.getDB().getLastError()
assert.eq( error, null )
assert.eq( coll.find().itcount(), 0 )

printjson( "Sharding-then-inserting-multikey tested, now trying inserting-then-sharding-multikey" )

// Insert a bunch of data then shard over key which is an array
var coll = mongos.getCollection( "" + coll + "2" )
for( var i = 0; i < 10; i++ ){
    // TODO : does not check weird cases like [ i, i ]
    coll.insert({ i : [ i, i + 1 ] })
    checkError( false )
}

coll.ensureIndex({ _id : 1, i : 1 })

try {
    st.shardColl( coll, { _id : 1, i : 1 },  { _id : ObjectId(), i : 1 } )
}
catch( e ){
    print( "Correctly threw error on sharding with multikey index." )
}

st.printShardingStatus()

// Insert a bunch of data then shard over key which is not an array
var coll = mongos.getCollection( "" + coll + "3" )
for( var i = 0; i < 10; i++ ){
    // TODO : does not check weird cases like [ i, i ]
    coll.insert({ i : i })
    checkError( false )
}

coll.ensureIndex({ _id : 1, i : 1 })

st.shardColl( coll, { _id : 1, i : 1 },  { _id : ObjectId(), i : 1 } )

st.printShardingStatus()



// Finish
st.stop()
