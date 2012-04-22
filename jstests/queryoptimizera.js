// Check that a warning message about doing a capped collection scan for a query with an _id
// constraint is printed at appropriate times.  SERVER-5353

function numWarnings() {
    logs = db.adminCommand( { getLog:"global" } ).log
    ret = 0;
    logs.forEach( function( x ) {
                 if ( x.match( warningMatchRegexp ) ) {
                 ++ret;
                 }
                 } );
    return ret;
}

collectionNameIndex = 0;

// Generate a collection name not already present in the log.
do {
    testCollectionName = 'jstests_queryoptimizer9__' + collectionNameIndex++;
    warningMatchString = 'unindexed _id query on capped collection.*collection: test.' +
    testCollectionName;
    warningMatchRegexp = new RegExp( warningMatchString );    
    
} while( numWarnings() > 0 );

t = db[ testCollectionName ];
t.drop();

notCappedCollectionName = testCollectionName + '_notCapped';

notCapped = db[ notCappedCollectionName ];
notCapped.drop();

db.createCollection( testCollectionName, { capped:true, size:1000 } );
db.createCollection( notCappedCollectionName, { autoIndexId:false } );

t.insert( {} );
notCapped.insert( {} );

oldNumWarnings = 0;

function assertNoNewWarnings() {
    assert.eq( oldNumWarnings, numWarnings() );
}

function assertNewWarning() {
    ++oldNumWarnings;
    assert.eq( oldNumWarnings, numWarnings() );
}

// Simple _id query without an _id index.
t.find( { _id:0 } ).itcount();
assertNewWarning();

// Simple _id query without an _id index, on a non capped collection.
notCapped.find( { _id:0 } ).itcount();
assertNoNewWarnings();

// A multi field query, including _id.
t.find( { _id:0, a:0 } ).itcount();
assertNewWarning();

// An unsatisfiable query.
t.find( { _id:0, a:{$in:[]} } ).itcount();
assertNoNewWarnings();

// An hinted query.
t.find( { _id:0 } ).hint( { $natural:1 } ).itcount();
assertNoNewWarnings();

// Retry a multi field query.
t.find( { _id:0, a:0 } ).itcount();
assertNewWarning();

// Warnings should not be printed when an index is added on _id.
t.ensureIndex( { _id:1 } );

t.find( { _id:0 } ).itcount();
assertNoNewWarnings();

t.find( { _id:0, a:0 } ).itcount();
assertNoNewWarnings();

t.find( { _id:0, a:0 } ).itcount();
assertNoNewWarnings();
