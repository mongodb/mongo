t = db.create_indexes;
t.drop();

// TODO: revisit this after createIndexes api stabilizes.
var isMongos = ("isdbgrid" == db.runCommand("ismaster").msg);
var extractResult = function(obj) {
    if (!isMongos) return obj;

    // Sample mongos format:
    // {
    //   raw: {
    //     "localhost:30000": {
    //       createdCollectionAutomatically: false,
    //       numIndexesBefore: 3,
    //       numIndexesAfter: 5,
    //       ok: 1
    //     }
    //   },
    //   ok: 1
    // }

    var numFields = 0;
    var result = null;
    for (var field in obj.raw) {
        result = obj.raw[field];
        numFields++;
    }

    assert.neq(null, result);
    assert.eq(1, numFields);
    return result;
};


res = t.runCommand( "createIndexes", { indexes : [ { key : { "x" : 1 }, name : "x_1" } ] } );
res = extractResult( res );
assert( res.createdCollectionAutomatically );
assert.eq( 1, res.numIndexesBefore );
assert.eq( 2, res.numIndexesAfter );

res = t.runCommand( "createIndexes", { indexes : [ { key : { "x" : 1 }, name : "x_1" } ] } );
res = extractResult( res );
assert.eq( 2, res.numIndexesBefore );
assert( res.noChangesMade );

res = t.runCommand( "createIndexes", { indexes : [ { key : { "x" : 1 }, name : "x_1" },
                                                   { key : { "y" : 1 }, name : "y_1" } ] } );
res = extractResult( res );
assert( !res.createdCollectionAutomatically );
assert.eq( 2, res.numIndexesBefore );
assert.eq( 3, res.numIndexesAfter );

res = t.runCommand( "createIndexes", { indexes : [ { key : { "a" : 1 }, name : "a_1" },
                                                   { key : { "b" : 1 }, name : "b_1" } ] } );
res = extractResult( res );
assert( !res.createdCollectionAutomatically );
assert.eq( 3, res.numIndexesBefore );
assert.eq( 5, res.numIndexesAfter );

res = t.runCommand( "createIndexes", { indexes : [ { key : { "a" : 1 }, name : "a_1" },
                                                   { key : { "b" : 1 }, name : "b_1" } ] } );
res = extractResult( res );
assert.eq( 5, res.numIndexesBefore );
assert( res.noChangesMade );

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
