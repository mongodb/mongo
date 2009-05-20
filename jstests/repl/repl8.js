// Test cloning of capped collections

baseName = "jstests_repl_repl8";

ports = allocatePorts( 2 );

m = startMongod( "--port", ports[ 0 ], "--dbpath", "/data/db/" + baseName + "-master", "--master", "--oplogSize", "1", "--nohttpinterface", "--bind_ip", "127.0.0.1" );
s = startMongod( "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:" + ports[ 0 ], "--nohttpinterface", "--bind_ip", "127.0.0.1" );

m.getDB( baseName ).createCollection( baseName, {capped:true,size:1000} );
assert.soon( function() { return s.getDB( baseName ).getCollection( baseName ).isCapped(); } );
