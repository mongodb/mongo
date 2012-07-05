//
// Runs most sharding passthrough tests using different kinds of multi-version clusters
//

var testsToIgnore = [ /dbadmin/,
                      /error1/,
                      /fsync/,
                      /fsync2/,
                      /geo.*/,
                      /indexh/,
                      /remove5/,
                      /update4/,
                      /loglong/,
                      /logpath/,
                      /notablescan/,
                      /compact.*/,
                      /check_shard_index/,
                      /bench_test.*/,
                      /mr_replaceIntoDB/,
                      /mr_auth/,
                      /queryoptimizera/ ]
              
var testsThatAreBuggy = [ /apply_ops1/,
                          /count5/,
                          /cursor8/,
                          /or4/,
                          /shellkillop/,
                          /update4/,
                          /profile\d*/,
                          /error3/,
                          /capped.*/,
                          /apitest_db/,
                          /cursor6/,
                          /copydb-auth/,
                          /profile\d*/,
                          /dbhash/,
                          /median/,
                          /apitest_dbcollection/,
                          /evalb/,
                          /evald/,
                          /eval_nolock/,
                          /auth1/,
                          /auth2/,
                          /dropdb_race/,
                          /unix_socket\d*/ ]
                     
function shouldRunTest( testFile, exceptPatterns ){
    
    if( /[\/\\]_/.test( testFile.name ) || // underscore tests
        ! /\.js$/.test( testFile.name ) )  // isn't a .js file
    { 
        print( " >>>>>>>> skipping " + x.name )
        return false
    }
    
    var testName = testFile.name.replace( /\.js$/, "" ).replace( /[\/\\]/, "" )
    
    for( var i = 0; i < testsToIgnore.length; i++ ){
        if( ! testsToIgnore[ i ].test( testName ) ) continue
        
        print( " >>>>>>>>> skipping test that would correctly fail under sharding " 
               + x.name )
        
        return false
    }
    
    
    for( var i = 0; i < testsThatAreBuggy.length; i++ ){
        if( ! testsThatAreBuggy[ i ].test( testName ) ) continue
        
        print( " !!!!!!!!!! skipping test that has failed under sharding but might not anymore " 
               + x.name)
        
        return false
    }
    
    if( exceptPatterns ){
        
        // Convert to array if not
        if( exceptPatterns.test ) exceptPatterns = [ exceptPatterns ]
        
        for( var i = 0; i < exceptPatterns.length; i++ ){
            if( ! exceptPatterns[ i ].test( testName ) ) continue
            
            print( " >>>>>>> skipping test that will fail due to a custom multi-version setup " 
                   + x.name)
            
            return false
        }
    }
    
    return true
}

var runMultiVersionTest = function( opts ){
    
    var oldOpts = {}
    
    oldOpts.mongosOptions = ShardingTest.mongosOptions
    oldOpts.rsOptions = ShardingTest.rsOptions 
    oldOpts.shardOptions = ShardingTest.shardOptions
    oldOpts.configOptions = ShardingTest.configOptions
    
    // Setup version context
    Object.extend( ShardingTest, opts )
    
    var st = new ShardingTest({ name : "sharding_mv_passthrough",
                                shards : 2,
                                verbose : 0, 
                                mongos : 1 })
    
    st.s.getDB( "admin" ).runCommand({ enableSharding : "test" })
    
    // Setup db variable context
    db = st.s.getDB( "test" )
    
    var files = listFiles( "jstests" )
    
    files.forEach( function( testFile ){
        
        if( ! shouldRunTest( testFile, opts.exceptPatterns ) ) return
        
        print( " *******************************************" )
        print( "         Test : " + testFile.name + " ..." )
        print( "                " + Date.timeFunc(
          function() {
              load( testFile.name );
          }, 1) + "ms" ) // end print
    })
    
    st.stop()
    
    // Reset version context
    
    Object.extend( ShardingTest, oldOpts )
}

//
// ACTUAL TESTS TO BE RUN
//

//
// Run multi-version tests of 2.0/2.2 mongos/mongod tests
//

jsTest.log( "Running multi-version 2.0/2.2 mongod/mongos passthrough tests..." )

runMultiVersionTest({
        shardOptions : { binVersion : "2.0.6" },
        rsOptions : { binVersion : "2.0.6" },
        mongosOptions : { binVersion : "latest" },
        configOptions : { binVersion : "2.0.6" } })

//
// Run multi-version passthrough for 2.2/2.0 mongod/mongos
//
        
jsTest.log( "Running multi-version 2.2/2.0 mongod/mongos passthrough tests..." )
        
runMultiVersionTest({
        shardOptions : { binVersion : "latest" },
        rsOptions : { binVersion : "latest" },
        mongosOptions : { binVersion : "2.0.6" },
        configOptions : { binVersion : "latest" },
        exceptPatterns : /.*auth.*/ }) // Can't run auth tests this way
        
        
jsTest.log( "Finished all multi-version tests..." )

