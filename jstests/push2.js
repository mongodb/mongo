
t = db.push2
t.drop()

t.save( { _id : 1 , a : [] } )

var s = "";
while ( s.length < 100000 )
    s += "asdasdasdasdasdasdasasdasdasdasdasdasdasasdasdasdasdasdasdasasdasdasdasdasdasdasasdasdasdasdasdasdas";

gotError = null;

for ( x=0; x<200; x++ ){
    t.update( {} , { $push : { a : s } } )
    gotError = db.getLastError();
    if ( gotError )
        break;
}

assert( gotError , "should have gotten error" );

t.drop();
