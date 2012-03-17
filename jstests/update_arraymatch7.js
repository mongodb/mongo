// Check that the positional operator works properly when an index only match is used for the update
// query spec.  SERVER-5067

t = db.jstests_update_arraymatch7;
t.drop();

function testPositionalInc() {
    t.remove();
    t.save( { a:[ { b:'match', count:0 } ] } );
    t.update( { 'a.b':'match' }, { $inc:{ 'a.$.count':1 } } );
    // Check that the positional $inc succeeded.
    assert( t.findOne( { 'a.count':1 } ) );
}

testPositionalInc();

// Now check with a non multikey index.
t.ensureIndex( { 'a.b' : 1 } );
testPositionalInc();
