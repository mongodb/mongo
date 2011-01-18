// test sorting, mainly a test ver simple with no index

t = db.sort2;
t.drop();

t.save({x:1, y:{a:5,b:4}});
t.save({x:1, y:{a:7,b:3}});
t.save({x:1, y:{a:2,b:3}});
t.save({x:1, y:{a:9,b:3}});

for( var pass = 0; pass < 2; pass++ ) {

    var res = t.find().sort({'y.a':1}).toArray();
    assert( res[0].y.a == 2 );
    assert( res[1].y.a == 5 );
    assert( res.length == 4 );

    t.ensureIndex({"y.a":1});

}

assert(t.validate().valid);
