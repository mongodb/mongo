t = db.group1;
t.drop();

t.save( { n : 1 , a : 1 } );
t.save( { n : 2 , a : 1 } );
t.save( { n : 3 , a : 2 } );
t.save( { n : 4 , a : 2 } );
t.save( { n : 5 , a : 2 } );


res = t.group( { key : { a : true } , 
                 reduce: function(obj,prev) { prev.count++; },
                 initial: { count: 0 }
               }
             );

printjson( res );
