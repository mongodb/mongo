// Test yielding group command SERVER-1395

t = db.jstests_group7;
t.drop();

a = 0;
for( i = 0; i < 3000; ++i ) {
	t.save( {a:a} );
}
db.getLastError();

// Iteratively update all a values atomically.
p = startParallelShell( 'for( a = 0; a < 300; ++a ) { db.jstests_group7.update( {$atomic:true}, {$set:{a:a}}, false, true ); db.getLastError(); }' );

assert.soon( function() {
            ret = t.group({key:{a:1},reduce:function(){},initial:{}});
            // Check if group sees more than one a value, indicating that it yielded.
            if ( ret.length > 1 ) {
                return true;
            }
            printjson( ret );
            return false;
            } );

p();
