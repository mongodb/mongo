
t = db.regex8;
t.drop()

t.insert( { _id : 1 , a : "abc" } )
t.insert( { _ud : 2 , a : "abc" } )
t.insert( { _id : 3 , a : "bdc" } )

function test( msg ){
    assert.eq( 3 , t.find().itcount() , msg + "1" )
    assert.eq( 2 , t.find( { a : /a.*/ } ).itcount() , msg + "2" )
    assert.eq( 3 , t.find( { a : /[ab].*/ } ).itcount() , msg + "3" )
    assert.eq( 3 , t.find( { a : /[a|b].*/ } ).itcount() , msg + "4" )
}

test( "A" );

t.ensureIndex( { a : 1 } )
test( "B" )
