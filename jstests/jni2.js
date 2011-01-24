
t = db.jni2;
t.remove( {} );

db.jni2t.remove( {} );

assert.eq( 0 , db.jni2t.find().length() , "A" );

t.save( { z : 1 } );
t.save( { z : 2 } );
assert.throws( function(){
    t.find( { $where : 
                       function(){ 
                           db.jni2t.save( { y : 1 } );
                           return 1; 
                       } 
            } ).forEach( printjson );
} , null , "can't save from $where" );

assert.eq( 0 , db.jni2t.find().length() , "B" )

assert(t.validate().valid , "E");
