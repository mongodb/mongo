// Test yielding group command SERVER-1395

t = db.jstests_group7;
t.drop();

a = 0;
for( i = 0; i < 3000; ++i ) {
	t.save( {a:a} );
}
db.getLastError();

assert.soon( function() {
            // Update all a values to a+1 atomically.
            p = startParallelShell( 'db.jstests_group7.update( {$atomic:true}, {$set:{a:' + ++a + '}}, false, true ); db.getLastError();' );
            // Check if group sees more than one a value, indicating that group yielded.
            ret = t.group({key:{a:1},reduce:function(){},initial:{}}).length > 1;
            p();
            return ret;
            } );
