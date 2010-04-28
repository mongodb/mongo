
t = db.datasize2
t.drop();

N = 1000
for ( i=0; i<N; i++ ){
    t.insert( { _id : i , s : "asdasdasdasdasdasdasd" } );
}

c = { dataSize : "test.datasize2" ,  
      "keyPattern" : {
          "_id" : 1
      },
      "min" : {
          "_id" : 0
      },
      "max" : {
          "_id" : N
      }
    };


assert.eq( N , db.runCommand( c ).numObjects , "A" )

c.maxObjects = 100;
assert.eq( 101 , db.runCommand( c ).numObjects , "B" )

