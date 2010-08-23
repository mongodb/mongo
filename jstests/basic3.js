
t = db.getCollection( "foo_basic3" );

t.find( { "a.b" : 1 } ).toArray();

ok = false;

try{
    t.save( { "a.b" : 5 } );
    ok = false;
}
catch ( e ){
    ok = true;
}
assert( ok , ". in names aren't allowed doesn't work" );

try{
    t.save( { "x" : { "a.b" : 5 } } );
    ok = false;
}
catch ( e ){
    ok = true;
}
assert( ok , ". in embedded names aren't allowed doesn't work" );
