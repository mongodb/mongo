
test = new SyncCCTest( "sync3" )//, { logpath : "/dev/null" } )

x = test._connections[0].getDB( "admin" ).runCommand( { "_testDistLockWithSyncCluster" : 1 , host : test.url } )
printjson( x )
assert( x.ok );



test.stop();
