// perform basic js tests in parallel

var files = listFiles("jstests");
var i = 0;
var argvs = new Array( [{0:[]}], [{1:[]}], [{2:[]}], [{3:[]}] );

// some tests can't run in parallel with others
var skipTests = { "jstests/dbadmin.js":1 };

files.forEach(
              function(x) {
              
              if ( /_runner/.test(x.name) ||
                  /_lodeRunner/.test(x.name) ||
                  ( x.name in skipTests ) ||
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
