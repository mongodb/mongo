
t = db.getCollection( "foo" );

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

