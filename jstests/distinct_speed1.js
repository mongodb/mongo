
t = db.distinct_speed1;

t.drop();
for ( var i=0; i<10000; i++ ){
    t.save( { x : i % 10 } );
}

assert.eq( 10 , t.distinct("x").length , "A1" );

function fast(){
    t.find().explain().millis;
}

function slow(){
    t.distinct("x");
}

for ( i=0; i<3; i++ ){
    print( "it: " + Date.timeFunc( fast ) );
    print( "di: " + Date.timeFunc( slow ) );
}


t.ensureIndex( { x : 1 } );
t.distinct( "x" , { x : 5 } )
