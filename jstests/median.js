f = db.jstests_median;
f.drop();

f.ensureIndex( {i:1} );
for( i = 0; i < 1000; ++i ) {
    f.save( {i:i} );
}

assert.eq( 500, db.runCommand( {medianKey:"test.jstests_median", keyPattern:{i:1}, min:{i:0}, max:{i:999} } ).median.i );
assert.eq( 0, db.runCommand( {medianKey:"test.jstests_median", keyPattern:{i:1}, min:{i:0}, max:{i:0} } ).median.i );
assert.eq( 500, db.runCommand( {medianKey:"test.jstests_median", keyPattern:{i:1}, min:{i:500}, max:{i:500} } ).median.i );
assert.eq( 1, db.runCommand( {medianKey:"test.jstests_median", keyPattern:{i:1}, min:{i:0}, max:{i:1} } ).median.i );
assert.eq( 1, db.runCommand( {medianKey:"test.jstests_median", keyPattern:{i:1}, min:{i:0}, max:{i:2} } ).median.i );

f.drop();
f.ensureIndex( {i:1,j:-1} );
for( i = 0; i < 100; ++i ) {
    for( j = 0; j < 100; ++j ) {
        f.save( {i:i,j:j} );
    }
}

assert.eq( 50, db.runCommand( {medianKey:"test.jstests_median", keyPattern:{i:1,j:-1}, min:{i:0,j:0}, max:{i:99,j:0} } ).median.i );
assert.eq( 0, db.runCommand( {medianKey:"test.jstests_median", keyPattern:{i:1,j:-1}, min:{i:0,j:0}, max:{i:0,j:0} } ).median.i );
assert.eq( 50, db.runCommand( {medianKey:"test.jstests_median", keyPattern:{i:1,j:-1}, min:{i:50,j:0}, max:{i:50,j:0} } ).median.i );
assert.eq( 1, db.runCommand( {medianKey:"test.jstests_median", keyPattern:{i:1,j:-1}, min:{i:0,j:0}, max:{i:1,j:0} } ).median.i );
assert.eq( 1, db.runCommand( {medianKey:"test.jstests_median", keyPattern:{i:1,j:-1}, min:{i:0,j:0}, max:{i:2,j:0} } ).median.i );

assert.eq( 49, db.runCommand( {medianKey:"test.jstests_median", keyPattern:{i:1,j:-1}, min:{i:0,j:99}, max:{i:0,j:0} } ).median.j );
assert.eq( 44, db.runCommand( {medianKey:"test.jstests_median", keyPattern:{i:1,j:-1}, min:{i:0,j:49}, max:{i:0,j:40} } ).median.j );

assert.eq( 10, db.runCommand( {medianKey:"test.jstests_median", keyPattern:{i:1,j:-1}, min:{i:10,j:50}, max:{i:11,j:75} } ).median.i );
assert.eq( 12, db.runCommand( {medianKey:"test.jstests_median", keyPattern:{i:1,j:-1}, min:{i:10,j:50}, max:{i:11,j:75} } ).median.j );
