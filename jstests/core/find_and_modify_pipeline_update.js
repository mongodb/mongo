/**
 * Tests the pipeline-style update is accepted by the findAndModify command.
 */
(function() {
    "use strict";

    const coll = db.find_and_modify_pipeline_update;
    coll.drop();

    assert.commandWorked(coll.insert([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}]));

    // Test that it generally works.
    let found = coll.findAndModify({query: {_id: 0}, update: [{$addFields: {y: 1}}]});
    assert.eq(found, {_id: 0});
    found = coll.findAndModify({query: {_id: 0}, update: [{$addFields: {z: 2}}], new: true});
    assert.eq(found, {_id: 0, y: 1, z: 2});

    // Test that it rejects the combination of arrayFilters and a pipeline-style update.
    let err = assert.throws(
        () => coll.findAndModify(
            {query: {_id: 1}, update: [{$addFields: {y: 1}}], arrayFilters: [{"i.x": 4}]}));
    assert.eq(err.code, ErrorCodes.FailedToParse);

    // SERVER-40404 Add support for fields.
    err = assert.throws(() => coll.findAndModify(
                            {query: {_id: 1}, update: [{$addFields: {y: 1}}], fields: {_id: 0}}));
    assert.eq(err.code, ErrorCodes.NotImplemented);

    // SERVER-40405 Add support for sort.
    err = assert.throws(() => coll.findAndModify(
                            {query: {_id: 1}, update: [{$addFields: {y: 1}}], sort: {_id: -1}}));
    assert.eq(err.code, ErrorCodes.NotImplemented);

    // SERVER-40401 Add support for bypassDocumentValidation.
    err = assert.throws(
        () => coll.findAndModify(
            {query: {_id: 1}, update: [{$addFields: {y: 1}}], bypassDocumentValidation: true}));
    assert.eq(err.code, ErrorCodes.NotImplemented);
}());
