(function() {
    'use strict';

    var isMongos = ("isdbgrid" == db.runCommand("ismaster").msg);

    var extractResult = function(obj) {
        if (!isMongos)
            return obj;

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

    var dbTest = db.getSisterDB('create_indexes_db');
    dbTest.dropDatabase();

    // Database does not exist
    var collDbNotExist = dbTest.create_indexes_no_db;
    var res = assert.commandWorked(
        collDbNotExist.runCommand('createIndexes', {indexes: [{key: {x: 1}, name: 'x_1'}]}));
    res = extractResult(res);
    assert(res.createdCollectionAutomatically);
    assert.eq(1, res.numIndexesBefore);
    assert.eq(2, res.numIndexesAfter);
    assert.isnull(res.note,
                  'createIndexes.note should not be present in results when adding a new index: ' +
                      tojson(res));

    // Collection does not exist, but database does
    var t = dbTest.create_indexes;
    var res = assert.commandWorked(
        t.runCommand('createIndexes', {indexes: [{key: {x: 1}, name: 'x_1'}]}));
    res = extractResult(res);
    assert(res.createdCollectionAutomatically);
    assert.eq(1, res.numIndexesBefore);
    assert.eq(2, res.numIndexesAfter);
    assert.isnull(res.note,
                  'createIndexes.note should not be present in results when adding a new index: ' +
                      tojson(res));

    // Both database and collection exist
    res = assert.commandWorked(
        t.runCommand('createIndexes', {indexes: [{key: {x: 1}, name: 'x_1'}]}));
    res = extractResult(res);
    assert(!res.createdCollectionAutomatically);
    assert.eq(2, res.numIndexesBefore);
    assert.eq(2,
              res.numIndexesAfter,
              'numIndexesAfter missing from createIndexes result when adding a duplicate index: ' +
                  tojson(res));
    assert(res.note,
           'createIndexes.note should be present in results when adding a duplicate index: ' +
               tojson(res));

    res = t.runCommand("createIndexes",
                       {indexes: [{key: {"x": 1}, name: "x_1"}, {key: {"y": 1}, name: "y_1"}]});
    res = extractResult(res);
    assert(!res.createdCollectionAutomatically);
    assert.eq(2, res.numIndexesBefore);
    assert.eq(3, res.numIndexesAfter);

    res = assert.commandWorked(t.runCommand(
        'createIndexes', {indexes: [{key: {a: 1}, name: 'a_1'}, {key: {b: 1}, name: 'b_1'}]}));
    res = extractResult(res);
    assert(!res.createdCollectionAutomatically);
    assert.eq(3, res.numIndexesBefore);
    assert.eq(5, res.numIndexesAfter);
    assert.isnull(res.note,
                  'createIndexes.note should not be present in results when adding new indexes: ' +
                      tojson(res));

    res = assert.commandWorked(t.runCommand(
        'createIndexes', {indexes: [{key: {a: 1}, name: 'a_1'}, {key: {b: 1}, name: 'b_1'}]}));

    res = extractResult(res);
    assert.eq(5, res.numIndexesBefore);
    assert.eq(5,
              res.numIndexesAfter,
              'numIndexesAfter missing from createIndexes result when adding duplicate indexes: ' +
                  tojson(res));
    assert(res.note,
           'createIndexes.note should be present in results when adding a duplicate index: ' +
               tojson(res));

    res = t.runCommand("createIndexes", {indexes: [{}]});
    assert(!res.ok);

    res = t.runCommand("createIndexes", {indexes: [{}, {key: {m: 1}, name: "asd"}]});
    assert(!res.ok);

    assert.eq(5, t.getIndexes().length);

    res = t.runCommand("createIndexes", {indexes: [{key: {"c": 1}, sparse: true, name: "c_1"}]});
    assert.eq(6, t.getIndexes().length);
    assert.eq(1,
              t.getIndexes()
                  .filter(function(z) {
                      return z.sparse;
                  })
                  .length);

    res = t.runCommand("createIndexes", {indexes: [{key: {"x": "foo"}, name: "x_1"}]});
    assert(!res.ok);

    assert.eq(6, t.getIndexes().length);

    res = t.runCommand("createIndexes", {indexes: [{key: {"x": 1}, name: ""}]});
    assert(!res.ok);

    assert.eq(6, t.getIndexes().length);

    // Test that v0 indexes cannot be created.
    res = t.runCommand('createIndexes', {indexes: [{key: {d: 1}, name: 'd_1', v: 0}]});
    assert.commandFailed(res, 'v0 index creation should fail');

    // Test that v1 indexes can be created explicitly.
    res = t.runCommand('createIndexes', {indexes: [{key: {d: 1}, name: 'd_1', v: 1}]});
    assert.commandWorked(res, 'v1 index creation should succeed');

    // Test that index creation fails with an invalid top-level field.
    res = t.runCommand('createIndexes', {indexes: [{key: {e: 1}, name: 'e_1'}], 'invalidField': 1});
    assert.commandFailedWithCode(res, ErrorCodes.BadValue);

    // Test that index creation fails with an invalid field in the index spec for index version V2.
    res = t.runCommand('createIndexes',
                       {indexes: [{key: {e: 1}, name: 'e_1', 'v': 2, 'invalidField': 1}]});
    assert.commandFailedWithCode(res, ErrorCodes.InvalidIndexSpecificationOption);

    // Test that index creation fails with an invalid field in the index spec for index version V1.
    res = t.runCommand('createIndexes',
                       {indexes: [{key: {e: 1}, name: 'e_1', 'v': 1, 'invalidField': 1}]});
    assert.commandFailedWithCode(res, ErrorCodes.InvalidIndexSpecificationOption);

}());
