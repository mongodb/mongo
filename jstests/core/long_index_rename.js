// SERVER-7720 Building an index with a too-long name is acceptable since 4.2.
// Previously, we would disallow index creation with with too long a name.
// @tags: [requires_non_retryable_commands, assumes_unsharded_collection]

(function() {
'use strict';

const coll = db.long_index_rename;
coll.drop();

for (let i = 1; i < 10; i++) {
    coll.save({a: i});
}

// Beginning with 4.2, index namespaces longer than 127 characters are acceptable.
assert.commandWorked(coll.createIndex({b: 1}, {name: 'a'.repeat(8192)}));

// Before 4.2, index namespace lengths were checked while renaming collections.
const dest = db.long_index_rename2;
dest.drop();
assert.commandWorked(coll.renameCollection(dest.getName()));
})();
