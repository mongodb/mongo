
t = db.apply_ops1;
t.drop();

//
// Input validation tests
//

// Empty array of operations.
assert.commandWorked(db.adminCommand({applyOps: []}),
                     'applyOps should not fail on empty array of operations');

// Non-array type for operations.
assert.commandFailed(db.adminCommand({applyOps: "not an array"}),
                     'applyOps should fail on non-array type for operations');

// Missing 'op' field in an operation.
assert.commandFailed(db.adminCommand({applyOps: [{ns: t.getFullName()}]}),
                     'applyOps should fail on operation without "op" field');

// Non-string 'op' field in an operation.
assert.commandFailed(db.adminCommand({applyOps: [{op: 12345, ns: t.getFullName()}]}),
                     'applyOps should fail on operation with non-string "op" field');

// Empty 'op' field value in an operation.
assert.commandFailed(db.adminCommand({applyOps: [{op: '', ns: t.getFullName()}]}),
                     'applyOps should fail on operation with empty "op" field value');

// Missing 'ns' field in an operation.
assert.commandFailed(db.adminCommand({applyOps: [{op: 'c'}]}),
                     'applyOps should fail on operation without "ns" field');

// Non-string 'ns' field in an operation.
assert.commandFailed(db.adminCommand({applyOps: [{op: 'c', ns: 12345}]}),
                     'applyOps should fail on operation with non-string "ns" field');

// Empty 'ns' field value in an operation of type 'n' (noop).
assert.commandWorked(db.adminCommand({applyOps: [{op: 'n', ns: ''}]}),
                     'applyOps should work on no op operation with empty "ns" field value');

// Empty 'ns' field value in operation type other than 'n'.
assert.commandFailed(db.adminCommand({applyOps: [{op: 'c', ns: ''}]}),
                     'applyOps should fail on non-"n" operation type with empty "ns" field value');

// Valid 'ns' field value in unknown operation type 'x'.
assert.commandFailed(db.adminCommand({applyOps: [{op: 'x', ns: t.getFullName()}]}),
                     'applyOps should fail on unknown operation type "x" with valid "ns" value');



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
assert.eq( true, res.results[1], "B6" );

