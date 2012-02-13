// Test precision of NumberLong values with v1 index code SERVER-3717

t = db.jstests_numberlong2;
t.drop();

t.ensureIndex( {x:1} );

function chk(longNum) {
    t.remove();
    t.save({ x: longNum });
    assert.eq(longNum, t.find().hint({ x: 1 }).next().x);
    assert.eq(longNum, t.find({}, { _id: 0, x: 1 }).hint({ x: 1 }).next().x);
}

chk( NumberLong("1123539983311657217") );
chk(NumberLong("-1123539983311657217"));
 chk(NumberLong("4503599627370495"));
 chk(NumberLong("4503599627370496"));
 chk(NumberLong("4503599627370497"));

t.remove();

s = "11235399833116571";
for( i = 99; i >= 0; --i ) {
    t.save( {x:NumberLong( s + i )} );
}

assert.eq( t.find().sort( {x:1} ).hint( {$natural:1} ).toArray(), t.find().sort( {x:1} ).hint( {x:1} ).toArray() );
