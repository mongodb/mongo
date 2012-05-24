
// test for SERVER-5040 - if documents move forward during an initial sync.

var rt = new ReplSetTest( { name : "replset7tests" , nodes: 1 } );

var nodes = rt.startSet();
rt.initiate();
var master = rt.getMaster();

var md = master.getDB( 'd' );
var mdc = md[ 'c' ];

// prep the data
var doccount = 50000;
for( i = 0; i < doccount; ++i ) {
    mdc.insert( { _id:i, x:i } );
}
md.getLastError();

mdc.ensureIndex( { x : 1 }, { unique: true } );
md.getLastError();

// add a secondary
var slave = rt.add();
rt.reInitiate();
print ("initiation complete!");
var sc = slave.getDB( 'd' )[ 'c' ];
slave.setSlaveOk();

// Wait for slave to start cloning.
//assert.soon( function() { c = sc.find( { _id:1, x:1 } ); print( c ); return c > 0; } );


// Move all documents to the end by growing it
for (i = 0; i < doccount; ++i) {
    mdc.remove( { _id:i, x:i } );
    mdc.insert( { _id:doccount+i, x:i, bigstring: "ayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayay" } );
    md.getLastError();
}

// Wait for replication to catch up.
rt.awaitSecondaryNodes();

// Do we have an index?
assert.eq (1, slave.getDB( 'd' )['system.indexes']
           .find({"v" : 1,"key" : {"x" : 1},"unique" : true,"ns" : "d.c","name" : "x_1"}).count());
