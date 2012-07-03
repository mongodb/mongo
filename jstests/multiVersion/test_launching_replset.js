//
// Tests launching multi-version ReplSetTest replica sets
//

var verifyVersion = function( mongo, version ){
    
    var result = mongo.getDB( "admin" ).runCommand({ serverStatus : 1 })
    
    if( result.version != version ) printjson( result  )
    
    assert.eq( result.version, version )
}

var versionsToCheck = [ "1.8.5",
                        "2.0.6" ]
                       
jsTest.log( "Testing legacy versions..." )

for( var i = 0; i < versionsToCheck.length; i++ ){

    var version = versionsToCheck[ i ]
    
    // Set up a replica set
    
    var rst = new ReplSetTest({ nodes : 2 })
    
    rst.startSet({ binVersion : version })
        
    var nodes = rst.nodes
    
    // Make sure the started versions are actually the correct versions
    for( var j = 0; j < nodes.length; j++ ) verifyVersion( nodes[j], version )
    
    rst.stopSet()
}

jsTest.log( "Testing mixed versions..." )

// Set up a multi-version replica set

var rst = new ReplSetTest({ nodes : 2 })

rst.startSet({ binVersion : versionsToCheck })

var nodes = rst.nodes

var getVersion = function( mongo ){
 var result = mongo.getDB( "admin" ).runCommand({ serverStatus : 1 })
 return result.version
}

var verifyAllVersionsFound = function( versionsFound ){
 for( var j = 0; j < versionsToCheck.length; j++ )
     assert( versionsFound[ versionsToCheck[j] ] )
}

//Make sure we have hosts of all the different versions
var versionsFound = {}
for( var j = 0; j < nodes.length; j++ ) 
    versionsFound[ getVersion( nodes[j] ) ] = true

verifyAllVersionsFound( versionsFound )

rst.stopSet()

jsTest.log( "Done!" )




//
// End
//
