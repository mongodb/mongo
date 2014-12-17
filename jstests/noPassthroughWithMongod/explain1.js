// SERVER-2662 - drop client cursor in a context where query will yield frequently

t = db.jstests_slowNightly_explain1;
t.drop();

// Periodically drops the collection, invalidating client cursors for s2's operations.
s1 = startParallelShell( "t = db.jstests_slowNightly_explain1; for( var i = 0; i < 80; ++i ) { t.drop(); t.ensureIndex({x:1}); for( var j = 0; j < 1000; ++j ) { t.save( {x:j,y:1} ) }; sleep( 100 ); }" );

// Query repeatedly.
s2 = startParallelShell( "t = db.jstests_slowNightly_explain1; for( var i = 0; i < 500; ++i ) { try { z = t.find( {x:{$gt:0},y:1} ).explain(); t.count( {x:{$gt:0},y:1} ); } catch( e ) {} }" );

// Put pressure on s2 to yield more often.
s3 = startParallelShell( "t = db.jstests_slowNightly_explain1; for( var i = 0; i < 200; ++i ) { t.validate({scandata:true}); }" );

s1();
s2();
s3();
