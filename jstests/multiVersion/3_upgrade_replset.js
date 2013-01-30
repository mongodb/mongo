//
// Tests upgrading a replica set
//

load( './jstests/multiVersion/libs/multi_rs.js' )
load( './jstests/libs/test_background_ops.js' )

var oldVersion = "2.0"
var newVersion = "2.2"

var nodes = { n1 : { binVersion : oldVersion }, 
              n2 : { binVersion : oldVersion },
              a3 : { binVersion : oldVersion } }

var rst = new ReplSetTest({ nodes : nodes })

rst.startSet()
rst.initiate()

// Wait for a primary node...
var primary = rst.getPrimary()
var otherOpConn = new Mongo( rst.getURL() )
var insertNS = "test.foo"


jsTest.log( "Starting parallel operations during upgrade..." )

function findAndInsert( rsURL, coll ){
    
    var coll = new Mongo( rsURL ).getCollection( coll + "" )
    var count = 0
    
    jsTest.log( "Starting finds and inserts..." )
    
    while( ! isFinished() ){
        
        try{
            
            coll.insert({ _id : count, hello : "world" })
            assert.eq( null, coll.getDB().getLastError() )
            assert.neq( null, coll.findOne({ _id : count }) )
        }
        catch( e ){
            printjson( e )
        }
        
        count++
    }
    
    jsTest.log( "Finished finds and inserts..." )
    return count
}

var joinFindInsert = 
    startParallelOps( primary, // The connection where the test info is passed and stored
                      findAndInsert,
                      [ rst.getURL(), insertNS ] )


jsTest.log( "Upgrading replica set..." )

rst.upgradeSet( "latest" )

jsTest.log( "Replica set upgraded." )

// Wait for primary
var primary = rst.getPrimary()

printjson( rst.status() )


// Allow more valid writes to go through
sleep( 10 * 1000 )


joinFindInsert()

var totalInserts = primary.getCollection( insertNS ).find().sort({ _id : -1 }).next()._id + 1
var dataFound = primary.getCollection( insertNS ).count()

jsTest.log( "Found " + dataFound + " docs out of " + tojson( totalInserts ) + " inserted." )

assert.gt( dataFound / totalInserts, 0.5 )

rst.stopSet()

