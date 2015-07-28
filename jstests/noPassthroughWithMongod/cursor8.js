// This should get skipped when testing replication.

var t = db.cursor8;
t.drop();
t.save( {} );
t.save( {} );
t.save( {} );

assert.eq( 3 , t.find().count() , "A0" );

var initialTotalOpen = db.serverStatus().metrics.cursor.open.total;

function test( want , msg ){
    var res = db.serverStatus().metrics.cursor;
    assert.eq(want + initialTotalOpen, res.open.total, msg + " " + tojson(res));
}

test( 0 , "A1" );
assert.eq( 3 , t.find().count() , "A2" );
assert.eq( 3 , t.find( {} ).count() , "A3" );
assert.eq( 2, t.find( {} ).limit( 2 ).itcount() , "A4" );
test( 1 , "B1" );
