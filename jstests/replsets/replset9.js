

var rt = new ReplSetTest( { name : "replset9tests" , nodes: 1, oplogSize: 400 } );

var nodes = rt.startSet();
rt.initiate();
var master = rt.getMaster();
var bigstring = "a";
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
    bigstring += "a";
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
rt.reInitiate();
print ("initiation complete!");
var sc = slave.getDB( 'd' )[ 'c' ];
slave.setSlaveOk();

print ("updating and deleting documents");
for (i = doccount*2; i > doccount; --i) {
    mdc.update( { _id:i }, { $inc: { x : 1 } } );
    md.getLastError();
    mdc.remove( { _id:i } );
    md.getLastError();
    mdc.insert( { bs:bigstring } );
    md.getLastError();
}
print ("finished");
// Wait for replication to catch up.
rt.awaitReplication(640000);
