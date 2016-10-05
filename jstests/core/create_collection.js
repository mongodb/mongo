// Tests for the "create" command.
(function() {
    'use strict';

    db.create_collection.drop();
    var res = db.createCollection('create_collection', {unknown: 1});
    assert.commandFailed(res);
    assert.eq(res.codeName, 'InvalidOptions');

    // maxTimeMS option is allowed.
    db.create_collection_maxTimeMS.drop();
    assert.commandWorked(db.createCollection('create_collection_maxTimeMS', {maxTimeMS: 5000}));

    // usePowerOf2Sizes option is allowed.
    db.create_collection_usePowerOf2Sizes.drop();
    assert.commandWorked(
        db.createCollection('create_collection_usePowerOf2Sizes', {usePowerOf2Sizes: true}));
})();
