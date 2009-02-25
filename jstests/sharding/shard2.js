// shard2.js

/**
* test basic sharding
*/

s = new ShardingTest( "shard2" , 2 , 5 );

db = s.getDB( "test" );

s.adminCommand( { partition : "test" } );
s.adminCommand( { shard : "test.foo" , key : { num : 1 } } );
assert.eq( 1 , s.config.sharding.count()  , "sanity check 1" );
assert.eq( 1, s.config.sharding.findOne().shards.length );

s.adminCommand( { split : "test.foo" , find : { num : 0 } } );
assert.eq( 1 , s.config.sharding.count() );
shard = s.config.sharding.findOne();
assert.eq( 2 , shard.shards.length );
assert.eq( shard.shards[0].server , shard.shards[1].server , "server should be the same after a split" );


db.foo.save( { num : 1 , name : "eliot" } );
db.foo.save( { num : 2 , name : "sara" } );
db.foo.save( { num : -1 , name : "joe" } );

s.adminCommand( "connpoolsync" );

assert.eq( 3 , s.getServer( "test" ).getDB( "test" ).foo.find().length() , "not right directly to db A" );
assert.eq( 3 , db.foo.find().length() );

primary = s.getServer( "test" ).getDB( "test" );
seconday = s.getOther( primary ).getDB( "test" );

assert.eq( 3 , primary.foo.find().length() , "primary wrong B" );
assert.eq( 0 , seconday.foo.find().length() , "seconday wrong C" );
assert.eq( 3 , db.foo.find().sort( { num : 1 } ).length() );

// NOTE: at this point we have 2 shard on 1 server

// test move shard
assert.throws( function(){ s.adminCommand( { moveshard : "test.foo" , find : { num : 1 } , to : primary.getMongo().name } ); } );
assert.throws( function(){ s.adminCommand( { moveshard : "test.foo" , find : { num : 1 } , to : "adasd" } ) } );

s.adminCommand( { moveshard : "test.foo" , find : { num : 1 } , to : seconday.getMongo().name } );
assert.eq( 1 , primary.foo.find().length() );
assert.eq( 2 , seconday.foo.find().length() );

assert.eq( 1 , s.config.sharding.count() );
shard = s.config.sharding.findOne();
assert.eq( 2 , shard.shards.length );
assert.neq( shard.shards[0].server , shard.shards[1].server , "servers should not be the same after the move" );

// test inserts go to right server/shard

db.foo.save( { num : 3 , name : "bob" } );
s.adminCommand( "connpoolsync" );
assert.eq( 1 , primary.foo.find().length() , "after move insert go wrong place?" );
assert.eq( 3 , seconday.foo.find().length() , "after move insert go wrong place?" );

db.foo.save( { num : -2 , name : "funny man" } );
s.adminCommand( "connpoolsync" );
assert.eq( 2 , primary.foo.find().length() , "after move insert go wrong place?" );
assert.eq( 3 , seconday.foo.find().length() , "after move insert go wrong place?" );


db.foo.save( { num : 0 , name : "funny guy" } );
s.adminCommand( "connpoolsync" );
assert.eq( 2 , primary.foo.find().length() , "boundary A" );
assert.eq( 4 , seconday.foo.find().length() , "boundary B" );

// findOne
assert.eq( "eliot" , db.foo.findOne( { num : 1 } ).name );
assert.eq( "funny man" , db.foo.findOne( { num : -2 } ).name );

// getAll
function sumQuery( c ){
    var sum = 0;
    c.toArray().forEach(
        function(z){
            sum += z.num;
        }
    );
    return sum;
}
assert.eq( 6 , db.foo.find().length() , "sharded query 1" );
assert.eq( 3 , sumQuery( db.foo.find() ) , "sharded query 2" );

// sort by num

assert.eq( 3 , sumQuery( db.foo.find().sort( { num : 1 } ) ) , "sharding query w/sort 1" );
assert.eq( 3 , sumQuery( db.foo.find().sort( { num : -1 } ) ) , "sharding query w/sort 2" );

printjson( db.foo.find().sort( { num : 1 } ).toArray() );

assert.eq( "funny man" , db.foo.find().sort( { num : 1 } )[0].name , "sharding query w/sort 3 order wrong" );
assert.eq( -2 , db.foo.find().sort( { num : 1 } )[0].num , "sharding query w/sort 4 order wrong" );

assert.eq( "bob" , db.foo.find().sort( { num : -1 } )[0].name , "sharding query w/sort 5 order wrong" );
assert.eq( 3 , db.foo.find().sort( { num : -1 } )[0].num , "sharding query w/sort 6 order wrong" );


// sory by name

function getNames( c ){
    return c.toArray().map( function(z){ return z.name; } );
}
correct = getNames( db.foo.find() ).sort();
assert.eq( correct , getNames( db.foo.find().sort( { name : 1 } ) ) );
correct = correct.reverse();
assert.eq( correct , getNames( db.foo.find().sort( { name : -1 } ) ) );

assert.eq( 3 , sumQuery( db.foo.find().sort( { name : 1 } ) ) , "sharding query w/non-shard sort 1" );
assert.eq( 3 , sumQuery( db.foo.find().sort( { name : -1 } ) ) , "sharding query w/non-shard sort 2" );


// sort by num multiple shards per server
s.adminCommand( { split : "test.foo" , middle : { num : 2 } } );
assert.eq( "funny man" , db.foo.find().sort( { num : 1 } )[0].name , "sharding query w/sort and another split 1 order wrong" );
assert.eq( "bob" , db.foo.find().sort( { num : -1 } )[0].name , "sharding query w/sort and another split 2 order wrong" );
assert.eq( "funny man" , db.foo.find( { num : { $lt : 100 } } ).sort( { num : 1 } ).arrayAccess(0).name , "sharding query w/sort and another split 3 order wrong" );

// getMore
assert.eq( 4 , db.foo.find().limit(-4).toArray().length , "getMore 1" );
function countCursor( c ){
    var num = 0;
    while ( c.hasNext() ){
        c.next();
        num++;
    }
    return num;
}
assert.eq( 6 , countCursor( db.foo.find()._exec() ) , "getMore 2" );
//assert.eq( 6 , countCursor( db.foo.find().limit(1)._exec() ) , "getMore 3" );

s.stop();
