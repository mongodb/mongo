//
// simple runner to run toplevel tests in jstests
//
var files = listFiles("jstests");

var runnerStart = new Date()

files.forEach(
    function(x) {
        
        if ( /[\/\\]_/.test(x.name) ||
             ! /\.js$/.test(x.name ) ){ 
            print(" >>>>>>>>>>>>>>> skipping " + x.name);
            return;
        }
        
        
        print(" *******************************************");
        print("         Test : " + x.name + " ...");
        print("                " + Date.timeFunc( function() { load(x.name); }, 1) + "ms");
        
    }
);


var runnerEnd = new Date()

print( "total runner time: " + ( ( runnerEnd.getTime() - runnerStart.getTime() ) / 1000 ) + "secs" )
