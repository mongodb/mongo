
t = db.getCollection( "foo_basic3" );

t.find( { "a.b" : 1 } ).toArray();

ok = true;

try{
    t.save( { "a.b" : 5 } );
    ok = false;
}
catch ( e ){
}
assert( ok , ". in names aren't allowed doesn't work" );

try{
    t.save( { "x" : { "a.b" : 5 } } );
    ok = false;
}
catch ( e ){
}
assert( ok , ". in embedded names aren't allowed doesn't work" );

// following tests make sure update keys are checked
t.save({"a": 0,"b": 1})
try {
    t.update({"a": 0}, {"b.b": 1});
    ok = false;
} catch (e) {}
assert( ok , "must deny '.' in key of update" );

// upsert with embedded doc
try {
    t.update({"a": 10}, {"b": { "c.c" : 1 }}, true);
    ok = false;
} catch (e) {}
assert( ok , "must deny '.' in key of update" );

// if it is a modifier, it should still go through
t.update({"a": 0}, {$set: { "c.c": 1}})
t.update({"a": 0}, {$inc: { "c.c": 1}})

// edge cases
try {
    t.update({"a": 0}, {"": { "c.c": 1}})
    ok = false;
} catch (e) {}
assert( ok , "must deny '.' in key of update" );
t.update({"a": 0}, {})

