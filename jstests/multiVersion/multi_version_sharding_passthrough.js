//
// Runs most sharding passthrough tests using different kinds of multi-version clusters
//

var testsToIgnore = [ /dbadmin/,
                      /error1/,
                      /features1/,
                      /fsync/,
                      /fsync2/,
                      /geo.*/,
                      /indexh/,
                      /remove5/,
                      /rename6/,
                      /reversecursor/,
                      /update4/,
                      /loglong/,
                      /logpath/,
                      /notablescan/,
                      /compact.*/,
                      /check_shard_index/,
                      /bench_test.*/,
                      /mr_replaceIntoDB/,
                      /mr_auth/,
                      /queryoptimizera/,
                      /regex_limit/, // Not compatible with mongod before 2.3
                      /indexStatsCommand/, // New in 2.3.1
                      /storageDetailsCommand/, // New in 2.3.1
                      /validate_user_documents/, // New in 2.3.2
                      /features2/ ]

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
        print( " >>>>>>>> skipping " + testFile.name )
        return false
    }
    
    var testName = testFile.name.replace( /\.js$/, "" )
    var testSuffixAt = Math.max( testName.lastIndexOf( "/" ), testName.lastIndexOf( "\\" ) )
    if( testSuffixAt >= 0 ) testName = testName.substring( testSuffixAt + 1 )
    
    for( var i = 0; i < testsToIgnore.length; i++ ){
        if( ! testsToIgnore[ i ].test( testName ) ) continue
        
        print( " >>>>>>>>> skipping test that would correctly fail under sharding " 
               + testName )
        
        return false
    }
    
    
    for( var i = 0; i < testsThatAreBuggy.length; i++ ){
        if( ! testsThatAreBuggy[ i ].test( testName ) ) continue
        
        print( " !!!!!!!!!! skipping test that has failed under sharding but might not anymore " 
               + testName )
        
        return false
    }
    
    if( exceptPatterns ){
        
        // Convert to array if not
        if( exceptPatterns.test ) exceptPatterns = [ exceptPatterns ]
        
        for( var i = 0; i < exceptPatterns.length; i++ ){
            if( ! exceptPatterns[ i ].test( testName ) ) continue
            
            print( " >>>>>>> skipping test that will fail due to a custom multi-version setup " 
                   + testName )
            
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
    
    // Needed for some legacy tests
    var oldMyShardingTest = typeof( myShardingTest ) == 'undefined' ? undefined : myShardingTest
    myShardingTest = st
    // End legacy
    
    st.s.getDB( "admin" ).runCommand({ enableSharding : "test" })
    
    // Setup db variable context
    db = st.s.getDB( "test" )
    db.shardedPassthrough = true    
    
    var files = listFiles( "jstests" )
    var errors = []   
    
    files.forEach( function( testFile ){
        
        if( ! shouldRunTest( testFile, opts.exceptPatterns ) ) return
        
        print( " *******************************************" )
        print( "         Test : " + testFile.name + " ..." )
        
        try {
            
            print( "                " + Date.timeFunc(
              function() {
                  load( testFile.name );
              }, 1) + "ms" ) // end print     
        }
        catch( e ){
            
            if( ! opts.continueOnError ) throw e
            
            print( "Caught error : " )
            printjson( e )
            
            errors.push( e )            
        }
        
    })
        
    st.stop()
    
    // Reset version context
    myShardingTest = oldMyShardingTest
    
    Object.extend( ShardingTest, oldOpts )
    
    return errors
}

//
// ACTUAL TESTS TO BE RUN
//

//
// Run multi-version tests of 2.0/2.2 mongos/mongod tests
//

/*
jsTest.log( "Running single version 2.2 mongod/mongos passthrough tests..." )

runMultiVersionTest({
        shardOptions : { binVersion : "latest" },
        rsOptions : { binVersion : "latest" },
        mongosOptions : { binVersion : "latest" },
        configOptions : { binVersion : "latest" } })
*/

// List of tests not working in 2.0.6

// git diff <rev1>..<rev2> --summary jstests/ |\
//   sed 's/.*jstests\/\(.*\)\.js/\/\^\1\$\/,/' |\
//   egrep -v "\/.*\/.*\/" | egrep "^\/"

// Tests added to v2.2
var v22Only = [ /^all3$/,
                /^all4$/,
                /^all5$/,
                /^arrayfind6$/,
                /^arrayfind7$/,
                /^arrayfind8$/,
                /^arrayfind9$/,
                /^arrayfinda$/,
                /^basicc$/,
                /^bench_test3$/,
                /^bulk_insert$/,
                /^capped_empty$/,
                /^capped_server2639$/,
                /^compact_speed_test$/,
                /^count6$/,
                /^count7$/,
                /^count8$/,
                /^count9$/,
                /^counta$/,
                /^countb$/,
                /^countc$/,
                /^coveredIndex3$/,
                /^coveredIndex4$/,
                /^coveredIndex5$/,
                /^currentop$/,
                /^db$/,
                /^elemMatchProjection$/,
                /^existsa$/,
                /^explain4$/,
                /^explain5$/,
                /^explain6$/,
                /^explain7$/,
                /^explain8$/,
                /^explain9$/,
                /^explaina$/,
                /^explainb$/,
                /^explainc$/,
                /^find9$/,
                /^find_and_modify_server6226$/,
                /^find_and_modify_server6254$/,
                /^find_and_modify_where$/,
                /^finda$/,
                /^geo_max$/,
                /^geo_multikey0$/,
                /^geo_multikey1$/,
                /^geo_uniqueDocs2$/,
                /^geo_update3$/,
                /^geo_update_btree$/,
                /^geo_update_btree2$/,
                /^geog$/,
                /^gle_shell_server5441$/,
                /^hashindex1$/,
                /^hashtest1$/,
                /^hostinfo$/,
                /^inb$/,
                /^index12$/,
                /^index13$/,
                /^indexx$/,
                /^indexy$/,
                /^indexz$/,
                /^jni1$/,
                /^jni2$/,
                /^jni3$/,
                /^jni4$/,
                /^jni5$/,
                /^jni7$/,
                /^jni8$/,
                /^jni9$/,
                /^js1$/,
                /^js2$/,
                /^js3$/,
                /^js4$/,
                /^js5$/,
                /^js7$/,
                /^js8$/,
                /^js9$/,
                /^loglong$/,
                /^logpath$/,
                /^median$/,
                /^memory$/,
                /^mr_noscripting$/,
                /^nin2$/,
                /^numberlong4$/,
                /^oro$/,
                /^orp$/,
                /^orq$/,
                /^orr$/,
                /^padding$/,
                /^profile4$/,
                /^queryoptimizer10$/,
                /^queryoptimizer4$/,
                /^queryoptimizer5$/,
                /^queryoptimizer8$/,
                /^queryoptimizer9$/,
                /^queryoptimizerb$/,
                /^queryoptimizerc$/,
                /^regex_util$/,
                /^regexb$/,
                /^remove10$/,
                /^removea$/,
                /^removeb$/,
                /^removec$/,
                /^rename5$/,
                /^rename_stayTemp$/,
                /^server1470$/,
                /^server5346$/,
                /^shelltypes$/,
                /^showdiskloc$/,
                /^sortb$/,
                /^sortc$/,
                /^sortd$/,
                /^sorte$/,
                /^sortf$/,
                /^sortg$/,
                /^sorth$/,
                /^sorti$/,
                /^sortj$/,
                /^sortk$/,
                /^sortl$/,
                /^sortm$/,
                /^update_arraymatch7$/,
                /^updateh$/,
                /^updatei$/,
                /^updatej$/,
                /^updatek$/,
                /^use_power_of_2$/,
                /^useindexonobjgtlt$/,
                /^where4$/,
              ]

// Edited in v22
v22Only.push( /^array4$/ )
v22Only.push( /^queryoptimizer7$/ )
v22Only.push( /^update_blank1$/ )
v22Only.push( /^mr2$/ )
v22Only.push( /^query1$/ )
v22Only.push( /^updatee$/ )
v22Only.push( /^orf$/ )
v22Only.push( /^pull$/ )
v22Only.push( /^pullall$/ )
v22Only.push( /^update_addToSet$/ )
v22Only.push( /^not2$/ )
v22Only.push( /^index_diag$/ )
v22Only.push( /^find_and_modify4$/ )
v22Only.push( /^update6$/ )
v22Only.push( /^indexp$/ )
v22Only.push( /^index_elemmatch1$/ )
v22Only.push( /^splitvector$/ )

// TODO
// mr_merge2 - causes segfault?

jsTest.log( "Running multi-version 2.0/2.2 mongod/mongos passthrough tests..." )

var errors = []

errors = errors.concat( 

    runMultiVersionTest({
        shardOptions : { binVersion : "2.0.6" },
        rsOptions : { binVersion : "2.0.6" },
        mongosOptions : { binVersion : "latest" },
        configOptions : { binVersion : "2.0.6" },
        exceptPatterns : v22Only,
        continueOnError : true })
)

//
// Run multi-version passthrough for 2.2/2.0 mongod/mongos
//

v22Only.push( /.*auth.*/ ) // Can't run auth tests

jsTest.log( "Running multi-version 2.2/2.0 mongod/mongos passthrough tests..." )

errors = errors.concat(    

    runMultiVersionTest({
        shardOptions : { binVersion : "latest" },
        rsOptions : { binVersion : "latest" },
        mongosOptions : { binVersion : "2.0.6" },
        configOptions : { binVersion : "latest" },
        exceptPatterns : v22Only,
        continueOnError : true })
)

/*
runMultiVersionTest({
        shardOptions : { binVersion : [ "2.0.6", "latest" ] },
        rsOptions : { binVersion : [ "2.0.6", "latest" ] },
        mongosOptions : { binVersion : [ "2.0.6", "latest" ] },
        configOptions : { binVersion : [ "2.0.6", "latest" ] },
        exceptPatterns : v22Only,
        continueOnError : false })
*/        
        
jsTest.log( "Finished all multi-version tests..." )

if( errors.length > 0 ){
    
    printjson( errors )
    throw errors
}
