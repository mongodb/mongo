// Test object id sorting.

function testObjectIdFind( db ) {
    r = db.ed_db_find2_oif;
    r.drop();

    for( i = 0; i < 3; ++i )
	r.save( {} );

    f = r.find().sort( { _id: 1 } );
    assert.eq( 3, f.count() );
    assert( f[ 0 ]._id < f[ 1 ]._id );
    assert( f[ 1 ]._id < f[ 2 ]._id );
}

testObjectIdFind( db );
