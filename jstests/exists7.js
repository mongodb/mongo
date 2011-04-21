
// Test that non boolean value types are allowed with $explain spec. SERVER-2322

t = db.jstests_explain7;
t.drop();

function testIntegerExistsSpec() {
    t.remove();
    t.save( {} );
    t.save( {a:1} );

    assert.eq( 1, t.count( {a:{$exists:1}} ) );
    assert.eq( 1, t.count( {a:{$exists:0}} ) );
}

testIntegerExistsSpec();
t.ensureIndex( {a:1} );
testIntegerExistsSpec();
