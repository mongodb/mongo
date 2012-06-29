// SERVER-4638 - this tests explicit undefined values
// This case is marked as a dup of SERVER-4674

t = db.server4638
t.drop();

for ( i=0; i<2; i++ ){
    //t.insert( { _id : i , x : i } ); // this version works
    t.insert( { _id : i , x : i , undef: undefined } );
}

db.getLastError();
res = t.aggregate( { $project : { x : 1 } } )
printjson(res)

assert(res.ok, 'server4638 failed');

res = t.aggregate( { $project : { undef : 1 } } )
printjson(res)

assert(res.ok, 'server4638 failed 2');
assert.eq(res.result[0].undef, undefined);
// assert.eq(typeof(res.result[0].undef), "undefined"); // Commented out due to SERVER-6102

