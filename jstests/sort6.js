
t = db.sort6;
t.drop();

t.insert({_id:1})
t.insert({_id:2})
t.insert({_id:3,c:1})
t.insert({_id:4,c:2})

function get( x ){
    return t.find().sort( { c : x } ).map( function(z){ return z._id; } );
}

//assert.eq( [4,3,2,1] ,  get( -1 ) , "A1"  )
assert.eq( [1,2,3,4] , get( 1 ) , "A2" )

t.ensureIndex( { c : 1 } );

assert.eq( [4,3,2,1] ,  get( -1 ) , "B1"  )
assert.eq( [1,2,3,4] , get( 1 ) , "B2" )
