//
// simple runner to run toplevel tests in jstests
//
var files = listFiles("jstests");

db.runCommand( "closeAllDatabases" );
prev = db.runCommand( "meminfo" );

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
        
        assert( db.getSisterDB( "admin" ).runCommand( "closeAllDatabases" ).ok == 1 , "closeAllDatabases failed" );
        var now = db.runCommand( "meminfo" );
        if ( now.virtual > prev.virtual )
            print( "    LEAK : " + prev.virtual + " -->> " + now.virtual );
        prev = now;
    }
);



db.runCommand( "closeAllDatabases" );
print( "END   : " + tojson( db.runCommand( "meminfo" ) ) );
