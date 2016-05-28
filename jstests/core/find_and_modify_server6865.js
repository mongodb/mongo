/**
 * Test that projection with a positional operator works with findAndModify
 * when remove=true or new=false, but not when new=true.
 */
(function() {
    'use strict';

    var collName = 'find_and_modify_server6865';
    var t = db.getCollection(collName);
    t.drop();

    /**
     * Asserts that the specified query and projection returns the expected
     * result, using both the find() operation and the findAndModify command.
     *
     *   insert -- document to insert after dropping collection t
     *   cmdObj -- arguments to the findAndModify command
     *
     *   expected -- the document 'value' expected to be returned after the
     *               projection is applied
     */
    function testFAMWorked(insert, cmdObj, expected) {
        t.drop();
        t.insert(insert);

        var res;

        if (!cmdObj['new']) {
            // Test that the find operation returns the expected result.
            res = t.findOne(cmdObj['query'], cmdObj['fields']);
            assert.eq(res, expected, 'positional projection failed for find');
        }

        // Test that the findAndModify command returns the expected result.
        res = t.runCommand('findAndModify', cmdObj);
        assert.commandWorked(res, 'findAndModify command failed');
        assert.eq(res.value, expected, 'positional projection failed for findAndModify');

        if (cmdObj['new']) {
            // Test that the find operation returns the expected result.
            res = t.findOne(cmdObj['query'], cmdObj['fields']);
            assert.eq(res, expected, 'positional projection failed for find');
        }
    }

    /**
     * Asserts that the specified findAndModify command returns an error.
     */
    function testFAMFailed(insert, cmdObj) {
        t.drop();
        t.insert(insert);

        var res = t.runCommand('findAndModify', cmdObj);
        assert.commandFailed(res, 'findAndModify command unexpectedly succeeded');
    }

    //
    // Delete operations
    //

    // Simple query that uses an inclusion projection.
    testFAMWorked({_id: 42, a: [1, 2], b: 3},
                  {query: {_id: 42}, fields: {_id: 0, b: 1}, remove: true},
                  {b: 3});

    // Simple query that uses an exclusion projection.
    testFAMWorked({_id: 42, a: [1, 2], b: 3, c: 4},
                  {query: {_id: 42}, fields: {a: 0, b: 0}, remove: true},
                  {_id: 42, c: 4});

    // Simple query that uses $elemMatch in the projection.
    testFAMWorked({
        _id: 42,
        b: [{name: 'first', value: 1}, {name: 'second', value: 2}, {name: 'third', value: 3}]
    },
                  {query: {_id: 42}, fields: {b: {$elemMatch: {value: 2}}}, remove: true},
                  {_id: 42, b: [{name: 'second', value: 2}]});

    // Query on an array of values while using a positional projection.
    testFAMWorked(
        {_id: 42, a: [1, 2]}, {query: {a: 2}, fields: {'a.$': 1}, remove: true}, {_id: 42, a: [2]});

    // Query on an array of objects while using a positional projection.
    testFAMWorked({
        _id: 42,
        b: [{name: 'first', value: 1}, {name: 'second', value: 2}, {name: 'third', value: 3}]
    },
                  {query: {_id: 42, 'b.name': 'third'}, fields: {'b.$': 1}, remove: true},
                  {_id: 42, b: [{name: 'third', value: 3}]});

    // Query on an array of objects while using a position projection.
    // Verifies that the projection {'b.$.value': 1} is treated the
    // same as {'b.$': 1}.
    testFAMWorked({
        _id: 42,
        b: [{name: 'first', value: 1}, {name: 'second', value: 2}, {name: 'third', value: 3}]
    },
                  {query: {_id: 42, 'b.name': 'third'}, fields: {'b.$.value': 1}, remove: true},
                  {_id: 42, b: [{name: 'third', value: 3}]});

    // Query on an array of objects using $elemMatch while using an inclusion projection.
    testFAMWorked({
        _id: 42,
        a: 5,
        b: [{name: 'john', value: 1}, {name: 'jess', value: 2}, {name: 'jeff', value: 3}]
    },
                  {
                    query: {b: {$elemMatch: {name: 'john', value: {$lt: 2}}}},
                    fields: {_id: 0, a: 5},
                    remove: true
                  },
                  {a: 5});

    // Query on an array of objects using $elemMatch while using the positional
    // operator in the projection.
    testFAMWorked({
        _id: 42,
        b: [{name: 'john', value: 1}, {name: 'jess', value: 2}, {name: 'jeff', value: 3}]
    },
                  {
                    query: {b: {$elemMatch: {name: 'john', value: {$lt: 2}}}},
                    fields: {_id: 0, 'b.$': 1},
                    remove: true
                  },
                  {b: [{name: 'john', value: 1}]});

    //
    // Update operations with new=false
    //

    // Simple query that uses an inclusion projection.
    testFAMWorked({_id: 42, a: [1, 2], b: 3},
                  {query: {_id: 42}, fields: {_id: 0, b: 1}, update: {$inc: {b: 1}}, new: false},
                  {b: 3});

    // Simple query that uses an exclusion projection.
    testFAMWorked({_id: 42, a: [1, 2], b: 3, c: 4},
                  {query: {_id: 42}, fields: {a: 0, b: 0}, update: {$set: {c: 5}}, new: false},
                  {_id: 42, c: 4});

    // Simple query that uses $elemMatch in the projection.
    testFAMWorked({
        _id: 42,
        b: [{name: 'first', value: 1}, {name: 'second', value: 2}, {name: 'third', value: 3}]
    },
                  {
                    query: {_id: 42},
                    fields: {b: {$elemMatch: {value: 2}}},
                    update: {$set: {name: '2nd'}},
                    new: false
                  },
                  {_id: 42, b: [{name: 'second', value: 2}]});

    // Query on an array of values while using a positional projection.
    testFAMWorked(
        {_id: 42, a: [1, 2]},
        {query: {a: 2}, fields: {'a.$': 1}, update: {$set: {'b.kind': 'xyz'}}, new: false},
        {_id: 42, a: [2]});

    // Query on an array of objects while using a positional projection.
    testFAMWorked({
        _id: 42,
        b: [{name: 'first', value: 1}, {name: 'second', value: 2}, {name: 'third', value: 3}]
    },
                  {
                    query: {_id: 42, 'b.name': 'third'},
                    fields: {'b.$': 1},
                    update: {$set: {'b.$.kind': 'xyz'}},
                    new: false
                  },
                  {_id: 42, b: [{name: 'third', value: 3}]});

    // Query on an array of objects while using $elemMatch in the projection,
    // where the matched array element is modified.
    testFAMWorked(
        {_id: 1, a: [{x: 1, y: 1}, {x: 1, y: 2}]},
        {query: {_id: 1}, fields: {a: {$elemMatch: {x: 1}}}, update: {$pop: {a: -1}}, new: false},
        {_id: 1, a: [{x: 1, y: 1}]});

    // Query on an array of objects using $elemMatch while using an inclusion projection.
    testFAMWorked({
        _id: 42,
        a: 5,
        b: [{name: 'john', value: 1}, {name: 'jess', value: 2}, {name: 'jeff', value: 3}]
    },
                  {
                    query: {b: {$elemMatch: {name: 'john', value: {$lt: 2}}}},
                    fields: {_id: 0, a: 5},
                    update: {$inc: {a: 6}},
                    new: false
                  },
                  {a: 5});

    // Query on an array of objects using $elemMatch while using the positional
    // operator in the projection.
    testFAMWorked({
        _id: 42,
        b: [{name: 'john', value: 1}, {name: 'jess', value: 2}, {name: 'jeff', value: 3}]
    },
                  {
                    query: {b: {$elemMatch: {name: 'john', value: {$lt: 2}}}},
                    fields: {_id: 0, 'b.$': 1},
                    update: {$set: {name: 'james'}},
                    new: false
                  },
                  {b: [{name: 'john', value: 1}]});

    //
    // Update operations with new=true
    //

    // Simple query that uses an inclusion projection.
    testFAMWorked({_id: 42, a: [1, 2], b: 3},
                  {query: {_id: 42}, fields: {_id: 0, b: 1}, update: {$inc: {b: 1}}, new: true},
                  {b: 4});

    // Simple query that uses an exclusion projection.
    testFAMWorked({_id: 42, a: [1, 2], b: 3, c: 4},
                  {query: {_id: 42}, fields: {a: 0, b: 0}, update: {$set: {c: 5}}, new: true},
                  {_id: 42, c: 5});

    // Simple query that uses $elemMatch in the projection.
    testFAMWorked({
        _id: 42,
        b: [{name: 'first', value: 1}, {name: 'second', value: 2}, {name: 'third', value: 3}]
    },
                  {
                    query: {_id: 42},
                    fields: {b: {$elemMatch: {value: 2}}},
                    update: {$set: {'b.1.name': '2nd'}},
                    new: true
                  },
                  {_id: 42, b: [{name: '2nd', value: 2}]});

    // Query on an array of values while using a positional projection.
    testFAMFailed(
        {_id: 42, a: [1, 2]},
        {query: {a: 2}, fields: {'a.$': 1}, update: {$set: {'b.kind': 'xyz'}}, new: true});

    // Query on an array of objects while using a positional projection.
    testFAMFailed({
        _id: 42,
        b: [{name: 'first', value: 1}, {name: 'second', value: 2}, {name: 'third', value: 3}]
    },
                  {
                    query: {_id: 42, 'b.name': 'third'},
                    fields: {'b.$': 1},
                    update: {$set: {'b.$.kind': 'xyz'}},
                    new: true
                  });

    // Query on an array of objects while using $elemMatch in the projection.
    testFAMWorked({
        _id: 42,
        b: [{name: 'first', value: 1}, {name: 'second', value: 2}, {name: 'third', value: 3}]
    },
                  {
                    query: {_id: 42},
                    fields: {b: {$elemMatch: {value: 2}}, c: 1},
                    update: {$set: {c: 'xyz'}},
                    new: true
                  },
                  {_id: 42, b: [{name: 'second', value: 2}], c: 'xyz'});

    // Query on an array of objects while using $elemMatch in the projection,
    // where the matched array element is modified.
    testFAMWorked(
        {_id: 1, a: [{x: 1, y: 1}, {x: 1, y: 2}]},
        {query: {_id: 1}, fields: {a: {$elemMatch: {x: 1}}}, update: {$pop: {a: -1}}, new: true},
        {_id: 1, a: [{x: 1, y: 2}]});

    // Query on an array of objects using $elemMatch while using an inclusion projection.
    testFAMWorked({
        _id: 42,
        a: 5,
        b: [{name: 'john', value: 1}, {name: 'jess', value: 2}, {name: 'jeff', value: 3}]
    },
                  {
                    query: {b: {$elemMatch: {name: 'john', value: {$lt: 2}}}},
                    fields: {_id: 0, a: 5},
                    update: {$inc: {a: 6}},
                    new: true
                  },
                  {a: 11});

    // Query on an array of objects using $elemMatch while using the positional
    // operator in the projection.
    testFAMFailed({
        _id: 42,
        b: [{name: 'john', value: 1}, {name: 'jess', value: 2}, {name: 'jeff', value: 3}]
    },
                  {
                    query: {b: {$elemMatch: {name: 'john', value: {$lt: 2}}}},
                    fields: {_id: 0, 'b.$': 1},
                    update: {$set: {name: 'james'}},
                    new: true
                  });

})();
