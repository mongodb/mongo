
t = db.scan_capped_id;
t.drop()

x = t.runCommand( "create" , { capped : true , size : 10000 } )
assert( x.ok )

for ( i=0; i<100; i++ )
    t.insert( { _id : i , x : 1 } )

function q() {
    return t.findOne( { _id : 5 } )
}

function u() {
    t.update( { _id : 5 } , { $set : { x : 2 } } );
    var gle = db.getLastError();
    if ( gle )
        throw gle;
}
    

// SERVER-3064
//assert.throws( q , [] , "A1" );
//assert.throws( u , [] , "B1" );

t.ensureIndex( { _id : 1 } )

assert.eq( 1 , q().x )
q()
u()

assert.eq( 2 , q().x )
