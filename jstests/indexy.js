// Test count matching when null key is generated even though it should not be matched in a standard query. SERVER-4529

t = db.jstests_indexy;
t.drop();

t.save({a:[{},{c:10}]});

assert.eq( 0, t.find({'a.c':null}).itcount() );
assert.eq( 0, t.find({'a.c':null}).count() );

t.ensureIndex({'a.c':1});

assert.eq( 0, t.find({'a.c':null}).itcount() );
if( 0 ) { // SERVER-4529
assert.eq( 0, t.find({'a.c':null}).count() );
}
