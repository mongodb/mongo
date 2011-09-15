// Check that $nin is the opposite of $in SERVER-3264

t = db.jstests_nin2;
t.drop();

// Check various operator types.
function checkOperators( array, inMatches ) {
    inCount = inMatches ? 1 : 0;
    notInCount = 1 - inCount;
    assert.eq( inCount, t.count( {foo:{$in:array}} ) );
    assert.eq( notInCount, t.count( {foo:{$not:{$in:array}}} ) );
    assert.eq( notInCount, t.count( {foo:{$nin:array}} ) );
    assert.eq( inCount, t.count( {foo:{$not:{$nin:array}}} ) );
}

t.save({});

assert.eq( 1, t.count( {foo:null} ) );
assert.eq( 0, t.count( {foo:{$ne:null}} ) );
assert.eq( 0, t.count( {foo:1} ) );

// Check matching null against missing field.
checkOperators( [null], true );
checkOperators( [null,1], true );
checkOperators( [1,null], true );

t.remove();
t.save({foo:null});

assert.eq( 1, t.count( {foo:null} ) );
assert.eq( 0, t.count( {foo:{$ne:null}} ) );
assert.eq( 0, t.count( {foo:1} ) );

// Check matching empty set.
checkOperators( [], false );

// Check matching null against missing null field.
checkOperators( [null], true );
checkOperators( [null,1], true );
checkOperators( [1,null], true );

t.remove();
t.save({foo:1});

assert.eq( 0, t.count( {foo:null} ) );
assert.eq( 1, t.count( {foo:{$ne:null}} ) );
assert.eq( 1, t.count( {foo:1} ) );

// Check matching null against 1.
checkOperators( [null], false );
checkOperators( [null,1], true );
checkOperators( [1,null], true );

t.remove();
t.save( {foo:[0,1]} );
// Check exact match of embedded array.
checkOperators( [[0,1]], true );

t.remove();
t.save( {foo:[]} );
// Check exact match of embedded empty array.
checkOperators( [[]], true );

t.remove();
t.save( {foo:'foo'} );
// Check regex match.
checkOperators( [/o/], true );
