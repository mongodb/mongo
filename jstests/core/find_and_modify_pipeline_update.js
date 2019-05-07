/**
 * Tests the pipeline-style update is accepted by the findAndModify command.
 * @tags: [requires_non_retryable_writes]
 */
(function() {
    "use strict";

    const coll = db.find_and_modify_pipeline_update;
    coll.drop();

    // Test that it generally works.
    assert.commandWorked(coll.insert([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}]));
    let found = coll.findAndModify({query: {_id: 0}, update: [{$addFields: {y: 1}}]});
    assert.eq(found, {_id: 0});
    found = coll.findAndModify({query: {_id: 0}, update: [{$addFields: {z: 2}}], new: true});
    assert.eq(found, {_id: 0, y: 1, z: 2});

    // Test that pipeline-style update supports the 'fields' argument.
    assert(coll.drop());
    assert.commandWorked(
        coll.insert([{_id: 0, x: 0}, {_id: 1, x: 1}, {_id: 2, x: 2}, {_id: 3, x: 3}]));
    found = coll.findAndModify({query: {_id: 0}, update: [{$addFields: {y: 0}}], fields: {x: 0}});
    assert.eq(found, {_id: 0});

    found = coll.findAndModify({query: {_id: 1}, update: [{$addFields: {y: 1}}], fields: {x: 1}});
    assert.eq(found, {_id: 1, x: 1});

    found = coll.findAndModify(
        {query: {_id: 2}, update: [{$addFields: {y: 2}}], fields: {x: 0}, new: true});
    assert.eq(found, {_id: 2, y: 2});

    found = coll.findAndModify(
        {query: {_id: 3}, update: [{$addFields: {y: 3}}], fields: {x: 1}, new: true});
    assert.eq(found, {_id: 3, x: 3});

    // Test that it rejects the combination of arrayFilters and a pipeline-style update.
    let err = assert.throws(
        () => coll.findAndModify(
            {query: {_id: 1}, update: [{$addFields: {y: 1}}], arrayFilters: [{"i.x": 4}]}));
    assert.eq(err.code, ErrorCodes.FailedToParse);

    // SERVER-40405 Add support for sort.
    err = assert.throws(() => coll.findAndModify(
                            {query: {_id: 1}, update: [{$addFields: {y: 1}}], sort: {_id: -1}}));
    assert.eq(err.code, ErrorCodes.NotImplemented);
}());
