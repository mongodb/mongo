// test sorting, mainly a test ver simple with no index

debug = function( s ){
    //print( s );
}

t = db.sorrrt;
t.drop();

t.save({x:3,z:33});
t.save({x:5,z:33});
t.save({x:2,z:33});
t.save({x:3,z:33});
t.save({x:1,z:33});

debug( "a" )
for( var pass = 0; pass < 2; pass++ ) {
    assert( t.find().sort({x:1})[0].x == 1 );
    assert( t.find().sort({x:1}).skip(1)[0].x == 2 );
    assert( t.find().sort({x:-1})[0].x == 5 );
    assert( t.find().sort({x:-1})[1].x == 3 );
    assert.eq( t.find().sort({x:-1}).skip(0)[0].x , 5 );
    assert.eq( t.find().sort({x:-1}).skip(1)[0].x , 3 );
    t.ensureIndex({x:1});

}

debug( "b" )
assert(t.validate().valid);


db.sorrrt2.drop();
db.sorrrt2.save({x:'a'});
db.sorrrt2.save({x:'aba'});
db.sorrrt2.save({x:'zed'});
db.sorrrt2.save({x:'foo'});

debug( "c" )

for( var pass = 0; pass < 2; pass++ ) { 
    debug( tojson( db.sorrrt2.find().sort( { "x" : 1 } ).limit(1).next() ) );
    assert.eq( "a" , db.sorrrt2.find().sort({'x': 1}).limit(1).next().x , "c.1" );
    assert.eq( "a" , db.sorrrt2.find().sort({'x': 1}).next().x , "c.2" );
    assert.eq( "zed" , db.sorrrt2.find().sort({'x': -1}).limit(1).next().x , "c.3" );
    assert.eq( "zed" , db.sorrrt2.find().sort({'x': -1}).next().x , "c.4" );
}

debug( "d" )

assert(db.sorrrt2.validate().valid);
