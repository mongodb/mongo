// Test correctness of replication while a secondary's get more requests are killed on the primary
// using killop.  SERVER-7952

numDocs = 1e5;

// Set up a replica set.
replTest = new ReplSetTest( { name:'test', nodes:3 } );
nodes = replTest.startSet();
replTest.initiate();
primary = replTest.getMaster();
secondary = replTest.getSecondary();
db = primary.getDB( 'test' );
db.test.save( { a:0 } );
replTest.awaitReplication();
assert.soon( function() { return secondary.getDB( 'test' ).test.count() == 1; } );

// Start a parallel shell to insert new documents on the primary.
inserter = startParallelShell(
    'for( i = 1; i < ' + numDocs + '; ++i ) { \
         db.test.save( { a:i } );             \
         sleep( 1 );                          \
     }                                        \
     db.getLastError();'
);

// Periodically kill replication get mores.
for( i = 0; i < 1e3; ++i ) {
    allOps = db.currentOp();
    for( j in allOps.inprog ) {
        op = allOps.inprog[ j ];
        if ( op.ns == 'local.oplog.rs' && op.op == 'getmore' ) {
            db.killOp( op.opid );
        }
    }
    sleep( 100 );
}

// Wait for the inserter to finish.
inserter();

assert.eq( numDocs, db.test.count() );

// Return true when the correct number of documents are present on the secondary.  Otherwise print
// which documents are missing and return false.
function allReplicated() {
    count = secondary.getDB( 'test' ).test.count();
    if ( count == numDocs ) {
        // Return true if the count is as expected.
        return true;
    }
    
    // Identify and print the missing a-values.
    foundSet = {};
    c = secondary.getDB( 'test' ).test.find();
    while( c.hasNext() ) {
        foundSet[ '' + c.next().a ] = true;
    }
    missing = [];
    for( i = 0; i < numDocs; ++i ) {
        if ( !( ( '' + i ) in foundSet ) ) {
            missing.push( i );
        }
    }
    print( 'count: ' + count + ' missing: ' + missing );
    return false;
}

// Wait for the correct number of (replicated) documents to be present on the secondary.
assert.soon( allReplicated );
