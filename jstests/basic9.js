
t = db.getCollection( "foo_basic9" );

t.save( { "foo$bar" : 5 } );

ok = false;

try{
    t.save( { "$foo" : 5 } );
    ok = false;
}
catch ( e ){
    ok = true;
}
assert( ok , "key names aren't allowed to start with $ doesn't work" );

try{
    t.save( { "x" : { "$foo" : 5 } } );
    ok = false;
}
catch ( e ){
    ok = true;
}
assert( ok , "embedded key names aren't allowed to start with $ doesn't work" );

