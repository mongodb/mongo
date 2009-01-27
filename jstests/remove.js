// remove.js
// unit test for db remove

db = connect( "test" );
t = db.removetest;

function f(n,dir) {
    t.ensureIndex({x:dir||1});
    for( i = 0; i < n; i++ ) t.save( { x:3, z:"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" } );

    t.remove({x:3});
    assert( t.findOne() == null );
    assert( t.validate().valid );
}

t.drop();
f(300, 1);

f(500, -1);

assert(t.validate().valid);