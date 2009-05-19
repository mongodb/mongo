t = db.quota1;

print( "starting quota1.a" );
assert.throws( 
    function(z){
        db.eval(
            function(){
                db.quota1a.save( { a : 1 } );
                var a = 5;
                while ( true ){
                    a += 2;
                }
            }
        )
    }
);
print( "done quota1.a" );

//print( "starting quota1.b" );
//assert.throws( 
//    function(z){
//        db.eval(
//            function(){
//                db.quota1b.save( { a : 1 } );
//                var a = 5;
//                assert( sleep( 150000 ) );
//            }
//        )
//    }
//);
//print( "done quota1.b" );
//
//print( "starting quota1.c" );
//assert.throws( 
//    function(z){
//        db.eval(
//            function(){
//                db.quota1c.save( { a : 1 } );
//                var a = 1;
//                while ( true ){
//                    a += 1;
//                    assert( sleep( 1000 ) );
//                }
//            }
//        )
//    }
//);
//print( "done quota1.c" );
