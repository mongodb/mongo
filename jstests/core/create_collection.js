// Tests for the "create" command.
(function() {
    'use strict';

    var res = db.createCollection('create_collection', {unknown: 1});
    assert.commandFailed(res);
    assert.eq(res.codeName, 'InvalidOptions');
})();
