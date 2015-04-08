// Sanity check for the showRecordId option.

var t = db.show_record_id;
t.drop();

function checkResults( arr ) {
    for( i in arr ) {
        a = arr[ i ];
        assert( a['$recordId'] );
    }
}

// Check query.
t.save( {} );
checkResults( t.find()._addSpecial("$showDiskLoc" , true).toArray() );
checkResults( t.find().showRecordId().toArray() );

// Check query and get more.
t.save( {} );
t.save( {} );
checkResults( t.find().batchSize( 2 )._addSpecial("$showDiskLoc" , true).toArray() );
checkResults( t.find().batchSize( 2 ).showRecordId().toArray() );

// Check with a covered index.
t.ensureIndex( { a:1 } );
checkResults
( t.find( {}, { _id:0, a:1 } ).hint( { a:1 } )._addSpecial("$showDiskLoc" , true).toArray() );
checkResults( t.find( {}, { _id:0, a:1 } ).hint( { a:1 } ).showRecordId().toArray() );

// Check with an idhack query.
t.drop();
t.save({_id: 0, a: 1});
checkResults( t.find( { _id: 0 } )._addSpecial("$showDiskLoc", true).toArray() );
checkResults( t.find( { _id: 0 } ).showRecordId().toArray() );
