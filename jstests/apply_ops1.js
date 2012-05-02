
t = db.apply_ops1;
t.drop();

assert.eq( 0 , t.find().count() , "A0" );
a = db.adminCommand( { applyOps : [ { "op" : "i" , "ns" : t.getFullName() , "o" : { _id : 5 , x : 17 } } ] } )
assert.eq( 1 , t.find().count() , "A1a" );
assert.eq( true, a.results[0], "A1b" );

o = { _id : 5 , x : 17 }
assert.eq( o , t.findOne() , "A2" );

res = db.runCommand( { applyOps : [ 
    { "op" : "u" , "ns" : t.getFullName() , "o2" : { _id : 5 } , "o" : { $inc : { x : 1 } } } ,
    { "op" : "u" , "ns" : t.getFullName() , "o2" : { _id : 5 } , "o" : { $inc : { x : 1 } } } 
] } )

o.x++;
o.x++;

assert.eq( 1 , t.find().count() , "A3" );
assert.eq( o , t.findOne() , "A4" );
assert.eq( true, res.results[0], "A1b" );
assert.eq( true, res.results[1], "A1b" );


res = db.runCommand( { applyOps : 
                       [ 
                           { "op" : "u" , "ns" : t.getFullName() , "o2" : { _id : 5 } , "o" : { $inc : { x : 1 } } } ,
                           { "op" : "u" , "ns" : t.getFullName() , "o2" : { _id : 5 } , "o" : { $inc : { x : 1 } } } 
                       ]
                       , 
                       preCondition : [ { ns : t.getFullName() , q : { _id : 5 } , res : { x : 19 } } ]
                     }  );

o.x++;
o.x++;

assert.eq( 1 , t.find().count() , "B1" );
assert.eq( o , t.findOne() , "B2" );
assert.eq( true, res.results[0], "B2a" );
assert.eq( true, res.results[1], "B2b" );


res = db.runCommand( { applyOps :
                       [
                           { "op" : "u" , "ns" : t.getFullName() , "o2" : { _id : 5 } , "o" : { $inc : { x : 1 } } } ,
                           { "op" : "u" , "ns" : t.getFullName() , "o2" : { _id : 5 } , "o" : { $inc : { x : 1 } } }
                       ]
                       ,
                       preCondition : [ { ns : t.getFullName() , q : { _id : 5 } , res : { x : 19 } } ]
                     }  );

assert.eq( 1 , t.find().count() , "B3" );
assert.eq( o , t.findOne() , "B4" );

res = db.runCommand( { applyOps :
                       [
                           { "op" : "u" , "ns" : t.getFullName() , "o2" : { _id : 5 } , "o" : { $inc : { x : 1 } } } ,
                           { "op" : "u" , "ns" : t.getFullName() , "o2" : { _id : 6 } , "o" : { $inc : { x : 1 } } }
                       ]
                     }  );

assert.eq( true, res.results[0], "B5" );
assert.eq( false, res.results[1], "B6" );
