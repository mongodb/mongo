// Test $all/$elemMatch/null matching - SERVER-4517

t = db.jstests_all5;
t.drop();

function checkMatch( doc ) {
    t.drop();
    t.save( doc );
    assert.eq( 1, t.count( {a:{$all:[{$elemMatch:{b:null}}]}} ) );
}

function checkNoMatch( doc ) {
    t.drop();
    t.save( doc );
    assert.eq( 0, t.count( {a:{$all:[{$elemMatch:{b:null}}]}} ) );
}

checkNoMatch( {} );
checkNoMatch( {a:1} );

if ( 0 ) { // SERVER-4517
checkMatch( {a:[]} );
checkMatch( {a:[1]} );
}

checkMatch( {a:[{}]} );
checkMatch( {a:[{c:1}]} );
checkMatch( {a:[{b:null}]} );
checkNoMatch( {a:[{b:1}]}, 0 );
