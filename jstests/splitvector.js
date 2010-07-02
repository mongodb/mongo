f = db.jstests_splitvector;
f.drop();

// --- missing paramters ---

assert.eq( false, db.runCommand( { splitVector: "test.jstests_splitvector" } ).ok );
assert.eq( false, db.runCommand( { splitVector: "test.jstests_splitvector" , maxChunkSize: 1} ).ok );

// --- missing index ---

assert.eq( false, db.runCommand( { splitVector: "test.jstests_splitvector" , keyPattern: {x:1} , maxChunkSize: 1 } ).ok );

// --- empty collection ---

f.ensureIndex( {x:1} );
assert.eq( [], db.runCommand( { splitVector: "test.jstests_splitvector" , keyPattern: {x:1} , maxChunkSize: 1 } ).splitKeys );
