// Sanity check for the $showDiskLoc option.

t = db.jstests_showdiskloc;
t.drop();

function checkResults( arr ) {
    for( i in arr ) {
        a = arr[ i ];
        assert( a['$diskLoc'] );
    }
}

// Check query.
t.save( {} );
checkResults( t.find()._addSpecial("$showDiskLoc" , true).toArray() );

// Check query and get more.
t.save( {} );
t.save( {} );
checkResults( t.find().batchSize( 2 )._addSpecial("$showDiskLoc" , true).toArray() );

// Check with a covered index.
t.ensureIndex( { a:1 } );
checkResults
( t.find( {}, { _id:0, a:1 } ).hint( { a:1 } )._addSpecial("$showDiskLoc" , true).toArray() );
