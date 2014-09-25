c = db.jstests_js9;
c.drop();

c.save( { a : 1 } );
c.save( { a : 2 } );


assert.eq( 2 , c.find().length() );
assert.eq( 2 , c.find().count() );


assert.eq( 2 , 
           db.eval( 
               function(){
                   num = 0;
                   db.jstests_js9.find().forEach(
                       function(z){
                           num++;
                       }
                   );
                   return num;
               }
           )
         )
