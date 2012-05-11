
t = db.ttl1;
t.drop();

now = (new Date()).getTime();

for ( i=0; i<24; i++ )
    t.insert( { x : new Date( now - ( 3600 * 1000 * i ) ) } );
db.getLastError();

assert.eq( 24 , t.count() );

t.ensureIndex( { x : 1 } , { expireAfterSeconds : 20000 } );

assert.soon( 
    function() {
        return t.count() < 24;
    }, "never deleted" , 120 * 1000
);

assert.eq( 0 , t.find( { x : { $lt : new Date( now - 20000000 ) } } ).count() );
assert.eq( 6 , t.count() );
