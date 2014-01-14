t = db.group1;
t.drop();

t.save( { n : 1 , a : 1 } );
t.save( { n : 2 , a : 1 } );
t.save( { n : 3 , a : 2 } );
t.save( { n : 4 , a : 2 } );
t.save( { n : 5 , a : 2 } );

var p = { key : { a : true } , 
    reduce : function(obj,prev) { prev.count++; },
          initial: { count: 0 }
        };

res = t.group( p );

assert( res.length == 2 , "A" );
assert( res[0].a == 1 , "B" );
assert( res[0].count == 2 , "C" );
assert( res[1].a == 2 , "D" );
assert( res[1].count == 3 , "E" );

assert.eq( res , t.groupcmd( p ) , "ZZ" );

ret = t.groupcmd( { key : {} , reduce : p.reduce , initial : p.initial } );
assert.eq( 1 , ret.length , "ZZ 2" );
assert.eq( 5 , ret[0].count , "ZZ 3" );

ret = t.groupcmd( { key : {} , reduce : function(obj,prev){ prev.sum += obj.n } , initial : { sum : 0 } } );
assert.eq( 1 , ret.length , "ZZ 4" );
assert.eq( 15 , ret[0].sum , "ZZ 5" );

t.drop();

t.save( { "a" : 2 } );
t.save( { "b" : 5 } );
t.save( { "a" : 1 } );
t.save( { "a" : 2 } );

c = {key: {a:1}, cond: {}, initial: {"count": 0}, reduce: function(obj, prev) { prev.count++; } };

assert.eq( t.group( c ) , t.groupcmd( c ) , "ZZZZ" );


t.drop();

t.save( { name : { first : "a" , last : "A" } } );
t.save( { name : { first : "b" , last : "B" } } );
t.save( { name : { first : "a" , last : "A" } } );


p = { key : { 'name.first' : true } , 
      reduce : function(obj,prev) { prev.count++; },
      initial: { count: 0 }
    };

res = t.group( p );
assert.eq( 2 , res.length , "Z1" );
assert.eq( "a" , res[0]['name.first'] , "Z2" )
assert.eq( "b" , res[1]['name.first'] , "Z3" )
assert.eq( 2 , res[0].count , "Z4" )
assert.eq( 1 , res[1].count , "Z5" )


