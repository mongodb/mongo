(function() {
    'use strict';

    var t = db.create_indexes;
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


    var res = assert.commandWorked(t.runCommand('createIndexes',
                                                {indexes: [{key: {x: 1}, name: 'x_1'}]}));
    res = extractResult( res );
    assert( res.createdCollectionAutomatically );
    assert.eq( 1, res.numIndexesBefore );
    assert.eq( 2, res.numIndexesAfter );
    assert.isnull(res.note,
                  'createIndexes.note should not be present in results when adding a new index: ' +
                  tojson(res));

    res = assert.commandWorked(t.runCommand('createIndexes',
                                            {indexes: [{key: {x: 1}, name: 'x_1'}]}));
    res = extractResult( res );
    assert.eq( 2, res.numIndexesBefore );
    assert.eq(2, res.numIndexesAfter,
              'numIndexesAfter missing from createIndexes result when adding a duplicate index: ' +
              tojson(res));
    assert(res.note,
           'createIndexes.note should be present in results when adding a duplicate index: ' +
           tojson(res));

    res = t.runCommand( "createIndexes", { indexes : [ { key : { "x" : 1 }, name : "x_1" },
                                                       { key : { "y" : 1 }, name : "y_1" } ] } );
    res = extractResult( res );
    assert( !res.createdCollectionAutomatically );
    assert.eq( 2, res.numIndexesBefore );
    assert.eq( 3, res.numIndexesAfter );

    res = assert.commandWorked(t.runCommand('createIndexes',
        {indexes: [{key: {a: 1}, name: 'a_1'}, {key: {b: 1}, name: 'b_1'}]}));
    res = extractResult( res );
    assert( !res.createdCollectionAutomatically );
    assert.eq( 3, res.numIndexesBefore );
    assert.eq( 5, res.numIndexesAfter );
    assert.isnull(res.note,
                  'createIndexes.note should not be present in results when adding new indexes: ' +
                  tojson(res));

    res = assert.commandWorked(t.runCommand('createIndexes',
        {indexes: [{key: {a: 1}, name: 'a_1'}, {key: {b: 1}, name: 'b_1'}]}));

    res = extractResult( res );
    assert.eq( 5, res.numIndexesBefore );
    assert.eq(5, res.numIndexesAfter,
              'numIndexesAfter missing from createIndexes result when adding duplicate indexes: ' +
              tojson(res));
    assert(res.note,
           'createIndexes.note should be present in results when adding a duplicate index: ' +
           tojson(res));

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

    //
    // Test that v0 indexes can only be created with mmapv1
    //
    res = t.runCommand('createIndexes',
                       {indexes: [{key: {d: 1}, name: 'd_1', v: 0}]});

    if (!isMongos) {
        var status = db.serverStatus();
        assert.commandWorked(status);
        if (status.storageEngine.name === 'mmapv1') {
            assert.commandWorked(res, 'v0 index creation should work for mmapv1');
        } else {
            assert.commandFailed(res, 'v0 index creation should fail for non-mmapv1 storage engines');
        }
    }

}());
