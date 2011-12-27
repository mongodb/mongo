// Test bulk inserts with sharding

// Setup randomized test
var seed = new Date().getTime()
// seed = 0

Random.srand( seed )
print( "Seeded with " + seed )


var st = new ShardingTest({ name : jsTestName(), shards : 4, chunksize : 1 })

// Turn off balancer initially
st.setBalancer( false )

// Setup sharded collection
var mongos = st.s0
var db = mongos.getDB( jsTestName() )
var coll = db.coll
st.shardColl( coll, { _id : 1 }, false )

// Insert lots of bulk documents
var numDocs = 1000000

var bulkSize = Math.floor( Random.rand() * 1000 ) + 2
bulkSize = 4000
var docSize = 128 /* bytes */
print( "\n\n\nBulk size is " + bulkSize )

var data = "x"
while( Object.bsonsize({ x : data }) < docSize ){
    data += data
}

print( "\n\n\nDocument size is " + Object.bsonsize({ x : data }) )

var docsInserted = 0;
var balancerOn = false;

while( docsInserted < numDocs ){
    
    var currBulkSize = ( numDocs - docsInserted > bulkSize ) ? bulkSize : ( numDocs - docsInserted )
    
    var bulk = []
    for( var i = 0; i < currBulkSize; i++ ){
        bulk.push({ hi : "there", at : docsInserted, i : i, x : x })
    }
    
    coll.insert( bulk )
    var result = db.getLastError( 1 )
    if( result != null ){
        printjson( result )
        throw result
    }
    
    if( Math.floor( docsInserted / 10000 ) != Math.floor( ( docsInserted + currBulkSize ) / 10000 ) ){
        print( "Inserted " + (docsInserted + currBulkSize) + " documents." )
        st.printShardingStatus()
    }
                
    docsInserted += currBulkSize
    
    if( docsInserted > numDocs / 2 && ! balancerOn ){
        print( "Turning on balancer after half documents inserted." )
        st.setBalancer( true )
        balancerOn = true;
    }
}

// Check we inserted all the documents
st.printShardingStatus()

var count = coll.find().count()
var itcount = count //coll.find().itcount()

print( "Inserted " + docsInserted + " count : " + count + " itcount : " + itcount )

st.setBalancer( true )
sleep( 10000 )

var count = coll.find().count()
var itcount = coll.find().itcount()

print( "Inserted " + docsInserted + " count : " + count + " itcount : " + itcount )


// SERVER-3645
// assert.eq( docsInserted, count )
assert.eq( docsInserted, itcount )

