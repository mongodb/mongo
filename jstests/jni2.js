
t = db.jni2;
t.remove( {} );

db.jni2t.remove( {} );

assert( 0 == db.jni2t.find().length() );

t.save( { z : 1 } );
t.save( { z : 2 } );
assert( 2 == t.find( { $where : 
                       function(){ 
                           db.jni2t.save( { y : 1 } );
                           return 1; 
                       } 
                     } ).length() );


assert( 2 == db.jni2t.find().length() );
assert( 1 == db.jni2t.find()[0].y );

assert( 2 == t.find( { $where : 
                       function(){ 
                           if ( db.jni2t.find().length() != 2 )
                               return 0;( { y : 1 } );
                           return 1; 
                       } 
                     } ).length() );

assert(t.validate().valid);
