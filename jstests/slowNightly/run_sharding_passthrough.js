s = new ShardingTest( "auto1" , 2 , 1 , 1 );
s.adminCommand( { enablesharding : "test" } );
db=s.getDB("test");

var files = listFiles("jstests");

var runnerStart = new Date()

files.forEach(
    function(x) {
        
// /(basic|update).*\.js$/
        if ( /[\/\\]_/.test(x.name) ||
             ! /\.js$/.test(x.name ) ){ 
            print(" >>>>>>>>>>>>>>> skipping " + x.name);
            return;
        }

	// Notes:

	// apply_ops1: nothing works, dunno why yet. SERVER-1439

	// copydb, copydb2: copyDatabase seems not to work at all in
	//                  the ShardingTest setup.  SERVER-1440

	// cursor8: cursorInfo different/meaningless(?) in mongos 
	//          closeAllDatabases may not work through mongos
	//          SERVER-1441
	//          deal with cursorInfo in mongos SERVER-1442

	// dbcase: Database names are case-insensitive under ShardingTest?
	//         SERVER-1443

	// These are all SERVER-1444
	// count5: limit() and maybe skip() may be unreliable
	// geo3: limit() not working, I think
	// or4: skip() not working?

	// shellkillop: dunno yet.  SERVER-1445

	// These should simply not be run under sharding:
	// dbadmin: Uncertain  Cut-n-pasting its contents into mongo worked.
	// error1: getpreverror not supported under sharding 
	// fsync, fsync2: isn't supported through mongos
	// remove5: getpreverror, I think. don't run
	// update4: getpreverror don't run

	// Around July 20, command passthrough went away, and these
	// commands weren't implemented:
	// clean cloneCollectionAsCapped copydbgetnonce dataSize
	// datasize dbstats deleteIndexes dropIndexes forceerror
	// getnonce logout medianKey profile reIndex repairDatabase
	// reseterror splitVector validate

	// These all fail due to missing commands:
	if (/[\/\\](apitest_db|apitest_dbcollection|auth1|auth2|basic1|basic2|capped1|capped3|capped4|copydb-auth|cursor6|datasize|datasize2|dbadmin|dbhash|drop|dropIndex|error3|evalb|find1|find3|in5|index1|index10|index2|index.*|jni1|jni2|jni3|jni4|jni8|median|multi2|profile1|recstore|remove|remove2|repair|sort1|sort2|sort4|sort5|sort_numeric|splitvector|unique2|update)\.js$/.test(x.name)) {
	    print(" !!!!!!!!!!!!!!! skipping test that fails under sharding (missing command) " + x.name)	    
	    return;
	}
	// These are bugs (some might be fixed now):
	if (/[\/\\](apply_ops1|count5|cursor8|or4|shellkillop|update4)\.js$/.test(x.name)) {
	    print(" !!!!!!!!!!!!!!! skipping test that has failed under sharding but might not anymore " + x.name)	    
	    return;
	}
	// These aren't supposed to get run under sharding:
	if (/[\/\\](dbadmin|error1|fsync|fsync2|geo.*|remove5|update4)\.js$/.test(x.name)) {
	    print(" >>>>>>>>>>>>>>> skipping test that would fail under sharding " + x.name)	    
	    return;
	}
        
        print(" *******************************************");
        print("         Test : " + x.name + " ...");
        print("                " + Date.timeFunc(
		  function() {
		      load(x.name);
		  }, 1) + "ms");
        
    }
);


var runnerEnd = new Date()

print( "total runner time: " + ( ( runnerEnd.getTime() - runnerStart.getTime() ) / 1000 ) + "secs" )
