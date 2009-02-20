//
// simple runner to run toplevel tests in jstests
//
var files = listFiles("jstests");

var dummyDb = db.getSisterDB( "dummyDBdummydummy" );

dummyDb.getSisterDB( "admin" ).runCommand( "closeAllDatabases" );
prev = dummyDb.runCommand( "meminfo" );

print( "START : " + tojson( prev ) );

files.forEach(
    function(x) {
        
        if ( /_runner/.test(x.name) ||
             /_lodeRunner/.test(x.name) ||
             /jni/.test(x.name) ||
             /eval/.test(x.name) ||
             /where/.test(x.name) ||
             ! /\.js$/.test(x.name ) ){ 
            print(" >>>>>>>>>>>>>>> skipping " + x.name);
            return;
        }
        
        
        print(" *******************************************");
        print("         Test : " + x.name + " ...");
        print("                " + Date.timeFunc( function() { load(x.name); }, 1) + "ms");
        
        assert( dummyDb.getSisterDB( "admin" ).runCommand( "closeAllDatabases" ).ok == 1 , "closeAllDatabases failed" );
        var now = dummyDb.runCommand( "meminfo" );
        if ( now.virtual > prev.virtual )
            print( "    LEAK : " + prev.virtual + " -->> " + now.virtual );
        prev = now;
    }
);



dummyDb.getSisterDB( "admin" ).runCommand( "closeAllDatabases" );
print( "END   : " + tojson( dummyDb.runCommand( "meminfo" ) ) );
