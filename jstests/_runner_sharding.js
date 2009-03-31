//
// simple runner to run toplevel tests in jstests
//
var files = listFiles("jstests/sharding");

var num = 0;

files.forEach(
    function(x) {
        
        if ( /_runner/.test(x.name) ||
             /_lodeRunner/.test(x.name) ||
             ! /\.js$/.test(x.name ) ){ 
            print(" >>>>>>>>>>>>>>> skipping " + x.name);
            return;
        }
        
        if ( num++ > 0 ){
            sleep( 1000 ); // let things fully come down
        }

        print(" *******************************************");
        print("         Test : " + x.name + " ...");
        try {
            print("                " + Date.timeFunc( function() { load(x.name); }, 1) + "ms");
        }
        catch ( e ){
            print( " ERROR on " + x.name + "!! " + e );
            throw e;
        }
        
    }
);


