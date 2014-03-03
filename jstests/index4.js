// index4.js


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


// this should fail, not allowed -- we confirm that.
t.ensureIndex( { instances : { pool : 1 } } );
assert.eq( 0, db.system.indexes.find( {ns:"test.index4",name:{$ne:"_id_"}} ).count(), "no indexes should be here yet");

t.ensureIndex( { "instances.pool" : 1 } );

sleep( 10 );

a = t.find( { instances : { pool : "prod1" } } );
assert( a.length() == 1, "len1" );
assert( a[0].name == "alleyinsider", "alley" );

assert(t.validate().valid, "valid" );
