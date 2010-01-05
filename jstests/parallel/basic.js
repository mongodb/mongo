// perform basic js tests in parallel

var files = listFiles("jstests");
var i = 0;
var argvs = new Array( [{0:[]}], [{1:[]}], [{2:[]}], [{3:[]}], [{4:[]}] );

seed = new Date().getTime();
print( "random seed: " + seed );
srand( seed );

makeKeys = function( a ) {
    var ret = {};
    for( var i in a ) {
        ret[ a[ i ] ] = 1;
    }
    return ret;
}

// some tests can't run in parallel with most others
var skipTests = makeKeys( [ "jstests/dbadmin.js",
                           "jstests/repair.js",
                           "jstests/cursor8.js",
                           "jstests/recstore.js",
                           "jstests/extent.js",
                           "jstests/indexb.js",
                           "jstests/profile1.js"] );

// some tests can't be run in parallel with each other
var serialTestsArr = [ "jstests/fsync.js",
                      "jstests/fsync2.js" ];
var serialTests = makeKeys( serialTestsArr );

//argvs[ 4 ][ 0 ][ 0 ] = serialTestsArr;

files = Array.shuffle( files );

files.forEach(
              function(x) {
              
              if ( /_runner/.test(x.name) ||
                  /_lodeRunner/.test(x.name) ||
                  ( x.name in skipTests ) ||
                  ( x.name in serialTests ) ||
                  ! /\.js$/.test(x.name ) ){ 
              print(" >>>>>>>>>>>>>>> skipping " + x.name);
              return;
              }
              
              argvs[ i % 4 ][0][ i % 4 ].push( x.name );
              ++i;
              }
);

test = function() {
    var args = argumentsToArray( arguments )[ 0 ];
    var suite = Object.keySet( args )[ 0 ];
    var args = args[ suite ];
    args.forEach(
                  function( x ) {
                  print("         S" + suite + " Test : " + x + " ...");
                  var time = Date.timeFunc( function() { load(x); }, 1);
                  print("         S" + suite + " Test : " + x + " " + time + "ms" );
                  }
                  );
}

assert.parallelTests( test, argvs, "one or more tests failed", true );
