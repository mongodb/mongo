
t = db.create_indexes;
t.drop();

res = t.runCommand( "createIndexes", { indexes : [ { key : { "x" : 1 }, name : "x_1" } ] } );
assert( res.createdCollectionAutomatically );
assert.eq( 1, res.numIndexesBefore );
assert.eq( 2, res.numIndexesAfter );

res = t.runCommand( "createIndexes", { indexes : [ { key : { "x" : 1 }, name : "x_1" } ] } );
assert.eq( 2, res.numIndexesBefore );
assert.isnull( res.numIndexesAfter );

res = t.runCommand( "createIndexes", { indexes : [ { key : { "x" : 1 }, name : "x_1" },
                                                   { key : { "y" : 1 }, name : "y_1" } ] } );
assert( !res.createdCollectionAutomatically );
assert.eq( 2, res.numIndexesBefore );
assert.eq( 3, res.numIndexesAfter );

res = t.runCommand( "createIndexes", { indexes : [ { key : { "a" : 1 }, name : "a_1" },
                                                   { key : { "b" : 1 }, name : "b_1" } ] } );
assert( !res.createdCollectionAutomatically );
assert.eq( 3, res.numIndexesBefore );
assert.eq( 5, res.numIndexesAfter );

res = t.runCommand( "createIndexes", { indexes : [ { key : { "a" : 1 }, name : "a_1" },
                                                   { key : { "b" : 1 }, name : "b_1" } ] } );
assert.eq( 5, res.numIndexesBefore );
assert.isnull( res.numIndexesAfter );

res = t.runCommand( "createIndexes", { indexes : [ {} ] } );
assert( !res.ok );

res = t.runCommand( "createIndexes", { indexes : [ {} , { key : { m : 1 }, name : "asd" } ] } );
assert( !res.ok );

assert.eq( 5, t.getIndexes().length );

res = t.runCommand( "createIndexes",
                    { indexes : [ { key : { "c" : 1 }, sparse : true, name : "c_1" } ] } )
assert.eq( 6, t.getIndexes().length );
assert.eq( 1, t.getIndexes().filter( function(z){ return z.sparse; } ).length );

res = t.runCommand( "createIndexes",
                    { indexes : [ { key : { "x" : "foo" }, name : "x_1" } ] } );
assert( !res.ok )

assert.eq( 6, t.getIndexes().length );
