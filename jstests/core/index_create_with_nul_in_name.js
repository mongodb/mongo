// SERVER-16672 disallow creating indexes with NUL bytes in the name

(function() {
    'use strict';

    var coll = db.create_index_with_nul_in_name;
    coll.drop();

    var idx = {key: {'a': 1}, name: 'foo\0bar', ns: coll.getFullName()};

    var res = coll.runCommand('createIndexes', {indexes: [idx]});
    assert.commandFailed(res, tojson(res));
    assert.eq(res.code, 67);  // CannotCreateIndex
}());
