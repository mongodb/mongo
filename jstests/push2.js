print( "Temporary diagnostics for 32-bit Windows pushall.js failures" );
print( "db.hostInfo()" );
printjson( db.hostInfo() );
print( "db.serverStatus()" );
printjson( db.serverStatus() );
print( "db.stats()" );
printjson( db.stats() );
print( "End of temporary diagnostics for 32-bit Windows pushall.js failures" );

t = db.push2
t.drop()

t.save( { _id : 1 , a : [] } )

var inc = "asdasdasdasdasdasdasasdasdasdasdasdasdasasdasdasdasdasdasdasasdasdasdasdasdasdasasdasdasdasdasdasdas";
var s = inc;
while ( s.length < 100000 )
    s += inc;

gotError = null;

for ( x=0; x<200; x++ ){
    t.update( {} , { $push : { a : s } } )
    gotError = db.getLastError();
    if ( gotError )
        break;
}

assert( gotError , "should have gotten error" );

t.drop();
