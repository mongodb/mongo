// Test $all/$elemMatch with missing field - SERVER-4492

t = db.jstests_all4;
t.drop();

function checkQuery( query, val ) {
    assert.eq( val, t.count(query) );
    assert( !db.getLastError() );
    assert.eq( val, t.find(query).itcount() );
    assert( !db.getLastError() );    
}

checkQuery( {a:{$all:[]}}, 0 );
checkQuery( {a:{$all:[1]}}, 0 );
checkQuery( {a:{$all:[{$elemMatch:{b:1}}]}}, 0 );

t.save({});
checkQuery( {a:{$all:[]}}, 0 );
checkQuery( {a:{$all:[1]}}, 0 );
checkQuery( {a:{$all:[{$elemMatch:{b:1}}]}}, 0 );

t.save({a:1});
checkQuery( {a:{$all:[]}}, 0 );
checkQuery( {a:{$all:[1]}}, 1 );
checkQuery( {a:{$all:[{$elemMatch:{b:1}}]}}, 0 );

t.save({a:[{b:1}]});
checkQuery( {a:{$all:[]}}, 0 );
checkQuery( {a:{$all:[1]}}, 1 );
checkQuery( {a:{$all:[{$elemMatch:{b:1}}]}}, 1 );
