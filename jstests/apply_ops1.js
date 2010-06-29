
t = db.apply_ops1;
t.drop();

assert.eq( 0 , t.find().count() , "A0" );
db.runCommand( { applyOps : [ { "op" : "i" , "ns" : t.getFullName() , "o" : { _id : 5 , x : 17 } } ] } )
assert.eq( 1 , t.find().count() , "A1" );

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

