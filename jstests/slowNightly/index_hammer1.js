
t = db.index_hammer1;
t.drop();

for ( i=0; i<10000; i++ )
    t.insert( { x : i , y : i } );
db.getLastError();

ops = []

for ( i=0; i<50; i++ ) 
    ops.push( { op : "find" , ns : t.getFullName() , query : { x : { $gt : 5000 } , y : { $gt : 5000 } } } )

ops[10] = { op : "createIndex" , ns : t.getFullName() , key : { x : 1 } }
ops[20] = { op : "createIndex" , ns : t.getFullName() , key : { y : 1 } }
ops[30] = { op : "dropIndex" , ns : t.getFullName() , key : { x : 1 } }
ops[40] = { op : "dropIndex" , ns : t.getFullName() , key : { y : 1 } }

res = benchRun( { ops : ops , parallel : 5 , seconds : 20 , host : db.getMongo().host } )
printjson( res )

assert.eq( 10000 , t.count() );

