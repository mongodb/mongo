f = db.jstests_datasize;
f.drop();

assert.eq( 0, db.runCommand( {datasize:"test.jstests_datasize"} ).size );
f.save( {_id:'c'} );
assert.eq( 16, db.runCommand( {datasize:"test.jstests_datasize"} ).size );
f.save( {_id:'fg'} );
assert.eq( 33, db.runCommand( {datasize:"test.jstests_datasize"} ).size );

f.drop();
f.ensureIndex( {_id:1} );
assert.eq( 0, db.runCommand( {datasize:"test.jstests_datasize"} ).size );
f.save( {_id:'c'} );
assert.eq( 16, db.runCommand( {datasize:"test.jstests_datasize"} ).size );
f.save( {_id:'fg'} );
assert.eq( 33, db.runCommand( {datasize:"test.jstests_datasize"} ).size );

assert.eq( 0, db.runCommand( {datasize:"test.jstests_datasize", min:{_id:'a'}} ).ok );

assert.eq( 33, db.runCommand( {datasize:"test.jstests_datasize", min:{_id:'a'}, max:{_id:'z' }} ).size );
assert.eq( 16, db.runCommand( {datasize:"test.jstests_datasize", min:{_id:'a'}, max:{_id:'d' }} ).size );
assert.eq( 16, db.runCommand( {datasize:"test.jstests_datasize", min:{_id:'a'}, max:{_id:'d' }, keyPattern:{_id:1}} ).size );
assert.eq( 17, db.runCommand( {datasize:"test.jstests_datasize", min:{_id:'d'}, max:{_id:'z' }, keyPattern:{_id:1}} ).size );

assert.eq( 16, db.runCommand( {datasize:"test.jstests_datasize", min:{_id:'c'}, max:{_id:'c' }} ).size );

assert.eq( 0, db.runCommand( {datasize:"test.jstests_datasize", min:{_id:'a'}, max:{_id:'d' }, keyPattern:{a:1}} ).ok );
