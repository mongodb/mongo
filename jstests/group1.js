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
