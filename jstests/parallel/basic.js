// perform basic js tests in parallel

var files = listFiles("jstests");
var i = 0;
var argvs = new Array( [], [], [], [] );

files.forEach(
              function(x) {
              
              if ( /_runner/.test(x.name) ||
                  /_lodeRunner/.test(x.name) ||
                  ! /\.js$/.test(x.name ) ){ 
              print(" >>>>>>>>>>>>>>> skipping " + x.name);
              return;
              }

              argvs[ i++ % 4 ].push( x.name );
              }
);

printjson( argvs );

test = function() {
    var args = argumentsToArray( arguments );
    args.forEach(
                  function( x ) {
                  print("         Test : " + x + " ...");
                  var time = Date.timeFunc( function() { load(x); }, 1);
                  print("         Test : " + x + " " + time + "ms" );
                  }
                  );
}

assert.parallelTests( test, argvs, "one or more tests failed" );
