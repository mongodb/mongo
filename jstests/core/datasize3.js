
t = db.datasize3;
t.drop()

function run( options ){
    var c = { dataSize : "test.datasize3" };
    if ( options )
        Object.extend( c , options );
    return db.runCommand( c );
}

t.insert( { x : 1 } )

a = run()
b = run( { estimate : true } )

assert.eq( a.size , b.size );


t.ensureIndex( { x : 1 } )

for ( i=2; i<100; i++ )
    t.insert( { x : i } )

a = run( { min : { x : 20 } , max : { x : 50 } } ).size
b = run( { min : { x : 20 } , max : { x : 50 } , estimate : true } ).size

ratio = Math.min( a , b ) / Math.max( a , b );

assert.lt( 0.97 , ratio , "sizes not equal a: " + a + " b: " + b );




