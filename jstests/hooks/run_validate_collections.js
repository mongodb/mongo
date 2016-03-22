// Runner for validateCollections that runs full validation on all collections when loaded into
// the mongo shell.
'use strict';

(function() {
    assert.eq(typeof db, 'object', 'Invalid `db` object, is the shell connected to a mongod?');
    load('jstests/hooks/validate_collections.js');  // For validateCollections

    var dbNames = db.getMongo().getDBNames();

    for (var dbName of dbNames) {
        if (!validateCollections(db.getSiblingDB(dbName), {full: true})) {
            throw new Error('Collection validation failed');
        }
    }
})();