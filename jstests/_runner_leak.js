//
// simple runner to run toplevel tests in jstests
//
var files = listFiles("jstests");

var dummyDb = db.getSisterDB( "dummyDBdummydummy" );

dummyDb.getSisterDB( "admin" ).runCommand( "closeAllDatabases" );
prev = dummyDb.serverStatus();

print( "START : " + tojson( prev ) );

files.forEach(
    function(x) {
        
        if ( /_runner/.test(x.name) ||
             /_lodeRunner/.test(x.name) ||
             ! /\.js$/.test(x.name ) ){ 
            print(" >>>>>>>>>>>>>>> skipping " + x.name);
            return;
        }
        
        
        print(" *******************************************");
        print("         Test : " + x.name + " ...");
        print("                " + Date.timeFunc( function() { load(x.name); }, 1) + "ms");
        
        assert( dummyDb.getSisterDB( "admin" ).runCommand( "closeAllDatabases" ).ok == 1 , "closeAllDatabases failed" );
        var now = dummyDb.serverStatus();
        var leaked = now.mem.virtual - prev.mem.virtual;
        if ( leaked > 0 ){
            print( "    LEAK : " + prev.mem.virtual + " -->> " + now.mem.virtual  );
            printjson( now );
            if ( leaked > 20 )
                throw -1;
        }
        prev = now;
    }
);



dummyDb.getSisterDB( "admin" ).runCommand( "closeAllDatabases" );
print( "END   : " + tojson( dummyDb.serverStatus() ) );
