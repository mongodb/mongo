// Test update modifier uassert during initial sync. SERVER-4781

if( 0 ) { // SERVER-4781

load("jstests/replsets/rslib.js");
basename = "jstests_initsync4";

print("1. Bring up set");
replTest = new ReplSetTest( {name: basename, nodes: 1} );
replTest.startSet();
replTest.initiate();

m = replTest.getMaster();
md = m.getDB("d");
mc = m.getDB("d")["c"];

print("2. Insert some data");
N = 500000;
for( i = 0; i < N; ++i ) {
    mc.save( {_id:i,a:{}} );
}
md.getLastError();

print("3. Make sure synced");
replTest.awaitReplication();

print("4. Bring up a new node");
ports = allocatePorts( 3 );
basePath = "/data/db/" + basename;
hostname = getHostName();

s = startMongodTest (ports[2], basename, false, {replSet : basename, oplogSize : 2} );

var config = replTest.getReplSetConfig();
config.version = 2;
config.members.push({_id:2, host:hostname+":"+ports[2]});
try {
    m.getDB("admin").runCommand({replSetReconfig:config});
}
catch(e) {
    print(e);
}
reconnect(s);

print("5. Wait for new node to start cloning");

s.setSlaveOk();
sc = s.getDB("d")["c"];

wait( function() { printjson( sc.stats() ); return sc.stats().count > 0; } );

print("6. Start updating documents on primary");
for( i = N-1; i >= N-10000; --i ) {
    // If the document is cloned as {a:1}, the {$set:{'a.b':1}} modifier will uassert.
    mc.update( {_id:i}, {$set:{'a.b':1}} );
    mc.update( {_id:i}, {$set:{a:1}} );    
}

print("7. Wait for new node to become SECONDARY");
wait(function() {
     var status = s.getDB("admin").runCommand({replSetGetStatus:1});
     printjson(status);
     return status.members &&
     (status.members[1].state == 2);
     });

}