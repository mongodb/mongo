//
// Tests launching multi-version ShardingTest clusters
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
    
    // Set up a cluster
    
    var st = new ShardingTest({ shards : 2, 
                                mongos : 2,
                                other : { 
                                    separateConfig : true,
                                    mongosOptions : { binVersion : version },
                                    configOptions : { binVersion : version },
                                    shardOptions : { binVersion : version }
                                } })
    
    var shards = [ st.shard0, st.shard1 ]
    var mongoses = [ st.s0, st.s1 ]
    var configs = [ st.config0 ]
    
    // Make sure the started versions are actually the correct versions
    for( var j = 0; j < shards.length; j++ ) verifyVersion( shards[j], version )
    for( var j = 0; j < mongoses.length; j++ ) verifyVersion( mongoses[j], version )
    for( var j = 0; j < configs.length; j++ ) verifyVersion( configs[j], version )
    
    st.stop()
}

jsTest.log( "Testing mixed versions..." )
        
// Set up a multi-version cluster

var st = new ShardingTest({ shards : 2,
                            mongos : 2,
                            other : {
                                
                                // Three config servers
                                separateConfig : true,
                                sync : true,
                                
                                mongosOptions : { binVersion : versionsToCheck },
                                configOptions : { binVersion : versionsToCheck },
                                shardOptions : { binVersion : versionsToCheck }
                                
                            } })
    
var shards = [ st.shard0, st.shard1 ]
var mongoses = [ st.s0, st.s1 ]
var configs = [ st.config0, st.config1, st.config2 ]

var getVersion = function( mongo ){
    var result = mongo.getDB( "admin" ).runCommand({ serverStatus : 1 })
    return result.version
}

var verifyAllVersionsFound = function( versionsFound ){
    for( var j = 0; j < versionsToCheck.length; j++ )
        assert( versionsFound[ versionsToCheck[j] ] )
}

// Make sure we have hosts of all the different versions
var versionsFound = {}
for( var j = 0; j < shards.length; j++ ) 
    versionsFound[ getVersion( shards[j] ) ] = true

verifyAllVersionsFound( versionsFound )

versionsFound = {}
for( var j = 0; j < mongoses.length; j++ ) 
    versionsFound[ getVersion( mongoses[j] ) ] = true

verifyAllVersionsFound( versionsFound )
    
versionsFound = {}
for( var j = 0; j < configs.length; j++ ) 
    versionsFound[ getVersion( configs[j] ) ] = true
    
verifyAllVersionsFound( versionsFound )
    
st.stop()


jsTest.log( "Testing mixed versions with replica sets..." )
        
// Set up a multi-version cluster w/ replica sets

var st = new ShardingTest({ shards : 2,
                            mongos : 2,
                            other : {
                                
                                // Three config servers
                                separateConfig : true,
                                sync : true,
                                
                                // Replica set shards
                                rs : true,
                                
                                mongosOptions : { binVersion : versionsToCheck },
                                configOptions : { binVersion : versionsToCheck },
                                rsOptions : { binVersion : versionsToCheck }
                                
                            } })
    
var nodesA = st.rs0.nodes
var nodesB = st.rs1.nodes
var mongoses = [ st.s0, st.s1 ]
var configs = [ st.config0, st.config1, st.config2 ]

var getVersion = function( mongo ){
    var result = mongo.getDB( "admin" ).runCommand({ serverStatus : 1 })
    return result.version
}

var verifyAllVersionsFound = function( versionsFound ){
    for( var j = 0; j < versionsToCheck.length; j++ )
        assert( versionsFound[ versionsToCheck[j] ] )
}

// Make sure we have hosts of all the different versions
var versionsFound = {}
for( var j = 0; j < nodesA.length; j++ ) 
    versionsFound[ getVersion( nodesA[j] ) ] = true

verifyAllVersionsFound( versionsFound )

versionsFound = {}
for( var j = 0; j < nodesB.length; j++ ) 
    versionsFound[ getVersion( nodesA[j] ) ] = true

verifyAllVersionsFound( versionsFound )

versionsFound = {}
for( var j = 0; j < mongoses.length; j++ ) 
    versionsFound[ getVersion( mongoses[j] ) ] = true

verifyAllVersionsFound( versionsFound )
    
versionsFound = {}
for( var j = 0; j < configs.length; j++ ) 
    versionsFound[ getVersion( configs[j] ) ] = true
    
verifyAllVersionsFound( versionsFound )
    
st.stop()

jsTest.log( "Done!" )

//
// End
//
