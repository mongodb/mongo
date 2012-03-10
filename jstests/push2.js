
t = db.push2
t.drop()

t.save( { _id : 1 , a : [] } )

s = new Array(700000).toString();

gotError = null;

for ( x=0; x<100; x++ ){
    print (x + " pushes");
    t.update( {} , { $push : { a : s } } );
    gotError = db.getLastError();
    if ( gotError )
        break;
}

assert( gotError , "should have gotten error" );

t.drop();
