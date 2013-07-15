

var rt = new ReplSetTest( { name : "replset9tests" , nodes: 1, oplogSize: 300 } );

var nodes = rt.startSet();
rt.initiate();
var master = rt.getMaster();
var bigstring = Array(5000).toString();
var md = master.getDB( 'd' );
var mdc = md[ 'c' ];

// idea: while cloner is running, update some docs and then immediately remove them.
// oplog will have ops referencing docs that no longer exist.

var doccount = 20000;
// Avoid empty extent issues
mdc.insert( { _id:-1, x:"dummy" } );

// Make this db big so that cloner takes a while.
print ("inserting bigstrings");
for( i = 0; i < doccount; ++i ) {
    mdc.insert( { _id:i, x:bigstring } );
}
md.getLastError();

// Insert some docs to update and remove
print ("inserting x");
for( i = doccount; i < doccount*2; ++i ) {
    mdc.insert( { _id:i, bs:bigstring, x:i } );
}
md.getLastError();

// add a secondary; start cloning
var slave = rt.add();
(function reinitiate() {
    var master  = rt.nodes[0];
    var c = master.getDB("local")['system.replset'].findOne();
    var config  = rt.getReplSetConfig();
    config.version = c.version + 1;
    var admin  = master.getDB("admin");
    var cmd     = {};
    var cmdKey  = 'replSetReconfig';
    var timeout = timeout || 30000;
    cmd[cmdKey] = config;
    printjson(cmd);

    assert.soon(function() {
        var result = admin.runCommand(cmd);
        printjson(result);
        return result['ok'] == 1;
    }, "reinitiate replica set", timeout);
})();


print ("initiation complete!");
var sc = slave.getDB( 'd' )[ 'c' ];
slave.setSlaveOk();

print ("updating and deleting documents");
for (i = doccount*4; i > doccount; --i) {
    mdc.update( { _id:i }, { $inc: { x : 1 } } );
    mdc.remove( { _id:i } );
    mdc.insert( { bs:bigstring } );
}
md.getLastError();
print ("finished");
// Wait for replication to catch up.
rt.awaitReplication(640000);
