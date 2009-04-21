// shard6.js

s = new ShardingTest( "shard6" , 2 , 0 , 1 );

s.adminCommand( { partition : "test" } );
s.adminCommand( { shard : "test.data" , key : { num : 1 } } );

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

s.stop();
