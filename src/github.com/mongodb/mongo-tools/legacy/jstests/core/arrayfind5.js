// Test indexed elemmatch of missing field.

t = db.jstests_arrayfind5;
t.drop();

function check( nullElemMatch ) {
    assert.eq( 1, t.find( {'a.b':1} ).itcount() );
    assert.eq( 1, t.find( {a:{$elemMatch:{b:1}}} ).itcount() );
    assert.eq( nullElemMatch ? 1 : 0 , t.find( {'a.b':null} ).itcount() );
    assert.eq( nullElemMatch ? 1 : 0, t.find( {a:{$elemMatch:{b:null}}} ).itcount() ); // see SERVER-3377    
}

t.save( {a:[{},{b:1}]} );
check( true );
t.ensureIndex( {'a.b':1} );
check( true );

t.drop();

t.save( {a:[5,{b:1}]} );
check( false );
t.ensureIndex( {'a.b':1} );
check( false );
