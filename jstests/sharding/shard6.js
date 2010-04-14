// shard6.js

s = new ShardingTest( "shard6" , 2 , 0 , 1 );

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.data" , key : { num : 1 } } );

db = s.getDB( "test" );

// we want a lot of data, so lets make a 50k string to cheat :)
bigString = "";
while ( bigString.length < 50000 )
    bigString += "this is a big string. ";

// ok, now lets insert a some data
var num = 0;
for ( ; num<100; num++ ){
    db.data.save( { num : num , bigString : bigString } );
}

assert.eq( 100 , db.data.find().toArray().length );

// limit

assert.eq( 77 , db.data.find().limit(77).itcount() , "limit test 1" );
assert.eq( 1 , db.data.find().limit(1).itcount() , "limit test 2" );
for ( var i=1; i<10; i++ ){
    assert.eq( i , db.data.find().limit(i).itcount() , "limit test 3a : " + i );
    assert.eq( i , db.data.find().skip(i).limit(i).itcount() , "limit test 3b : " + i );
}

function assertOrder( start , num ){
    var a = db.data.find().skip(start).limit(num).sort( { num : 1 } ).map( function(z){ return z.num; } );
    var c = []
    for ( var i=0; i<num; i++ )
        c.push( start + i );
    assert.eq( c , a , "assertOrder start: " + start + " num: " + num );
}

assertOrder( 0 , 10 );
assertOrder( 5 , 10 );

assert.eq( 5 , db.data.find().skip( num - 5 ).itcount() , "skip 1 " );
assert.eq( 5 , db.data.find().skip( num - 5 ).sort( { num : 1 } ).itcount() , "skip 2 " );
assert.eq( 5 , db.data.find().skip( num - 5 ).sort( { _id : 1 } ).itcount() , "skip 3 " );
assert.eq( 0 , db.data.find().skip( num + 5 ).sort( { num : 1 } ).itcount() , "skip 4 " );
assert.eq( 0 , db.data.find().skip( num + 5 ).sort( { _id : 1 } ).itcount() , "skip 5 " );


// --- test save support ---

o = db.data.findOne();
o.x = 16;
db.data.save( o );
assert.eq( 16 , db.data.findOne( { _id : o._id } ).x , "x1 - did save fail?" );

s.stop();
