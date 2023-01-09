// SERVER-16672 disallow creating indexes with NUL bytes in the name

(function() {
'use strict';

var coll = db.create_index_with_nul_in_name;
coll.drop();

const idx = {
    key: {'a': 1},
    name: 'foo\0bar'
};
assert.commandFailedWithCode(coll.runCommand('createIndexes', {indexes: [idx]}),
                             ErrorCodes.CannotCreateIndex);
}());
