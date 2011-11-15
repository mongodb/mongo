// Test for SERVER-4158 (version changes during mapreduce)

var st = new ShardingTest({ shards : 2, mongos : 1, verbose : 2 })

//Stop balancer, since it'll just get in the way of these
st.stopBalancer()

var coll = st.s.getCollection( jsTest.name() + ".coll" )

var numDocs = 1000000
var numKeys = 1000
var numTests = 3

for( var i = 0; i < numDocs; i++ ){
    coll.insert({ _id : i, key : "" + ( i % numKeys ), value : i % numKeys })
}

assert.eq( null, coll.getDB().getLastError() )
assert.eq( numDocs, coll.find().itcount() )

var halfId = coll.find().itcount() / 2

// Shard collection in half
st.shardColl( coll, { _id : 1 }, { _id : halfId } )

st.printShardingStatus()

jsTest.log( "Collection now initialized with keys and values..." )

jsTest.log( "Starting migrations..." )

var migrateOp = { op : "command", ns : "admin", command : { moveChunk : "" + coll } }

var checkMigrate = function(){ print( "Result of migrate : " ); printjson( this ) }

var ops = {}
for( var i = 0; i < st._shardServers.length; i++ ){
    for( var j = 0; j < 2; j++ ){
        ops[ "" + (i * 2 + j) ] = { op : "command", ns : "admin", 
                                    command : { moveChunk : "" + coll, 
                                                     find : { _id : ( j == 0 ? 0 : halfId ) },
                                                       to : st._shardServers[i].shardName } } // , check : checkMigrate }
        // TODO:  Deadlock due to global V8Lock between scopes if we stop with a js check
    }
}

var bid = benchStart({ ops : ops,
                       host : st.s.host,
                       parallel : 1,
                       handleErrors : false })

jsTest.log( "Starting m/r..." )
                       
var map = function(){ emit( this.key, this.value ) }
var reduce = function(k, values){ 
    var total = 0
    for( var i = 0; i < values.length; i++ ) total += values[i]
    return total
}

var outputColl = st.s.getCollection( jsTest.name() + ".mrOutput" )

jsTest.log( "Output coll : " + outputColl )

for( var t = 0; t < numTests; t++ ){

    var results = coll.mapReduce( map, reduce, { out : { replace : outputColl.getName() } })
    
    // Assert that the results are actually correct, all keys have values of (numDocs / numKeys) x key
    var output = outputColl.find().sort({ _id : 1 }).toArray()
    
    // printjson( output )
    
    assert.eq( output.length, numKeys )
    printjson( output )
    for( var i = 0; i < output.length; i++ )
        assert.eq( parseInt( output[i]._id ) * ( numDocs / numKeys ), output[i].value )
    
}

jsTest.log( "Finishing parallel migrations..." )

printjson( benchFinish( bid ) )

st.stop()
