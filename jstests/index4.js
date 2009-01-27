// index4.js

db = connect( "test" );

t = db.index4;
t.drop();

t.save( { name : "alleyinsider" ,
          instances : [
              { pool : "prod1" } ,
              { pool : "dev1" }
          ]
        } );

t.save( { name : "clusterstock" ,
          instances : [
              { pool : "dev1" }
          ]
        } );

t.ensureIndex( { instances : { pool : 1 } } );
sleep( 10 );

a = t.find( { instances : { pool : "prod1" } } );
assert( a.length() == 1 );
assert( a[0].name == "alleyinsider" );

assert(t.validate().valid);