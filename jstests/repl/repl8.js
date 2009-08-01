// Test cloning of capped collections

baseName = "jstests_repl_repl8";

ports = allocatePorts( 2 );

m = startMongod( "--port", ports[ 0 ], "--dbpath", "/data/db/" + baseName + "-master", "--master", "--oplogSize", "1", "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );

m.getDB( baseName ).createCollection( "first", {capped:true,size:1000} );
assert( m.getDB( baseName ).getCollection( "first" ).isCapped() );

s = startMongod( "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:" + ports[ 0 ], "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );

assert.soon( function() { return s.getDB( baseName ).getCollection( "first" ).isCapped(); } );

m.getDB( baseName ).createCollection( "second", {capped:true,size:1000} );
assert.soon( function() { return s.getDB( baseName ).getCollection( "second" ).isCapped(); } );

m.getDB( baseName ).getCollection( "third" ).save( { a: 1 } );
assert.soon( function() { return s.getDB( baseName ).getCollection( "third" ).exists(); } );
assert.commandWorked( m.getDB( "admin" ).runCommand( {renameCollection:"jstests_repl_repl8.third", to:"jstests_repl_repl8.third_rename"} ) );
assert( m.getDB( baseName ).getCollection( "third_rename" ).exists() );
assert( !m.getDB( baseName ).getCollection( "third" ).exists() );
assert.soon( function() { return s.getDB( baseName ).getCollection( "third_rename" ).exists(); } );
assert.soon( function() { return !s.getDB( baseName ).getCollection( "third" ).exists(); } );

m.getDB( baseName ).getCollection( "fourth" ).save( {a:1} );
assert.commandWorked( m.getDB( baseName ).getCollection( "fourth" ).convertToCapped( 1000 ) );
assert( m.getDB( baseName ).getCollection( "fourth" ).isCapped() );
assert.soon( function() { return s.getDB( baseName ).getCollection( "fourth" ).isCapped(); } );
