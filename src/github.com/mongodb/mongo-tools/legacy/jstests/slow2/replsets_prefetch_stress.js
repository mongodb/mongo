// Replication prefetching stress test.  Insert many documents, each with a large number of multikey
// values on the same index.  All multikey keys will be generated, but only the first will be
// prefetched from the index.

var replTest = new ReplSetTest( { name:'testSet', nodes:3 } );
var nodes = replTest.startSet();
replTest.initiate();
var master = replTest.getMaster();
c = master.getDB( 'd' )[ 'c' ];

assert.writeOK(c.insert({ _id: 0 }));
replTest.awaitReplication();

// Create a:1 index.
c.ensureIndex( { a:1 } );

// Create an array of multikey values.
multikeyValues = [];
for( i = 0; i < 10000; ++i ) {
    multikeyValues.push( i );
}

// Insert documents with multikey values.
var bulk = c.initializeUnorderedBulkOp();
for( i = 0; i < 1000; ++i ) {
    bulk.insert({ a: multikeyValues });
}
assert.writeOK(bulk.execute());
replTest.awaitReplication(300000);

// Check document counts on all nodes.  On error a node might go down or fail to sync all data, see
// SERVER-6538.
assert.eq( 1001, c.count() );
nodes.forEach( function( node ) {
                   node.setSlaveOk();
                   assert.eq( 1001, node.getDB( 'd' )[ 'c' ].count() );
               } );
