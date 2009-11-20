// features2.js

s = new ShardingTest( "features2" , 2 , 1 , 1 );
s.adminCommand( { enablesharding : "test" } );

a = s._connections[0].getDB( "test" );
b = s._connections[1].getDB( "test" );

db = s.getDB( "test" );

// ---- distinct ----

db.foo.save( { x : 1 } );
db.foo.save( { x : 2 } );
db.foo.save( { x : 3 } );

assert.eq( "1,2,3" , db.foo.distinct( "x" ) , "distinct 1" );
assert( a.foo.distinct("x").length == 3 || b.foo.distinct("x").length == 3 , "distinct 2" );
assert( a.foo.distinct("x").length == 0 || b.foo.distinct("x").length == 0 , "distinct 3" );

assert.eq( 1 , s.onNumShards( "foo" ) , "A1" );

s.shardGo( "foo" , { x : 1 } , { x : 2 } , { x : 3 } );

assert.eq( 2 , s.onNumShards( "foo" ) , "A2" );

assert.eq( "1,2,3" , db.foo.distinct( "x" ) , "distinct 4" );

// ----- delete ---

assert.eq( 3 , db.foo.count() , "D1" );

db.foo.remove( { x : 3 } );
assert.eq( 2 , db.foo.count() , "D2" );

db.foo.save( { x : 3 } );
assert.eq( 3 , db.foo.count() , "D3" );

db.foo.remove( { x : { $gt : 2 } } );
assert.eq( 2 , db.foo.count() , "D4" );

db.foo.remove( { x : { $gt : -1 } } );
assert.eq( 0 , db.foo.count() , "D5" );

db.foo.save( { x : 1 } );
db.foo.save( { x : 2 } );
db.foo.save( { x : 3 } );
assert.eq( 3 , db.foo.count() , "D6" );
db.foo.remove( {} );
assert.eq( 0 , db.foo.count() , "D7" );

// --- _id key ---

db.foo2.save( { _id : new ObjectId() } );
db.foo2.save( { _id : new ObjectId() } );
db.foo2.save( { _id : new ObjectId() } );

assert.eq( 1 , s.onNumShards( "foo2" ) , "F1" );

s.adminCommand( { shardcollection : "test.foo2" , key : { _id : 1 } } );

assert.eq( 3 , db.foo2.count() , "F2" )
db.foo2.insert( {} );
assert.eq( 4 , db.foo2.count() , "F3" )


// --- map/reduce

db.mr.save( { x : 1 , tags : [ "a" , "b" ] } );
db.mr.save( { x : 2 , tags : [ "b" , "c" ] } );
db.mr.save( { x : 3 , tags : [ "c" , "a" ] } );
db.mr.save( { x : 4 , tags : [ "b" , "c" ] } );

m = function(){
    this.tags.forEach(
        function(z){
            emit( z , { count : 1 } );
        }
    );
};

r = function( key , values ){
    var total = 0;
    for ( var i=0; i<values.length; i++ ){
        total += values[i].count;
    }
    return { count : total };
};

doMR = function( n ){
    var res = db.mr.mapReduce( m , r );
    printjson( res );
    var x = db[res.result];
    assert.eq( 3 , x.find().count() , "MR T1 " + n );
    
    var z = {};
    x.find().forEach( function(a){ z[a._id] = a.value.count; } );
    assert.eq( 3 , z.keySet().length , "MR T2 " + n );
    assert.eq( 2 , z.a , "MR T2 " + n );
    assert.eq( 3 , z.b , "MR T2 " + n );
    assert.eq( 3 , z.c , "MR T2 " + n );

    x.drop();
}

doMR( "before" );

assert.eq( 1 , s.onNumShards( "mr" ) , "E1" );
s.shardGo( "mr" , { x : 1 } , { x : 2 } , { x : 3 } );
assert.eq( 2 , s.onNumShards( "mr" ) , "E1" );

doMR( "after" );

s.stop();
