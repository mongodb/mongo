// perform basic js tests in parallel

var files = listFiles("jstests");

var params = new Array( [], [], [], [] );

Random.setRandomSeed();

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

params[ 0 ] = serialTestsArr;

files = Array.shuffle( files );

var i = 0;
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
              
              params[ i % 4 ].push( x.name );
              ++i;
              }
);

// randomize ordering of the serialTests
params[ 0 ] = Array.shuffle( params[ 0 ] );

t = new ParallelTester();

for( var i in params ) {
    params[ i ].unshift( i );
    t.add( ParallelTester.fileTester, params[ i ] );
}

t.run( "one or more tests failed", true );
