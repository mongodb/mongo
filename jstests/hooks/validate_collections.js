// Wrapper around the validate command that can be used to validate index key counts.
'use strict';

function validateCollections(db, obj) {
    function dumpCollection(coll, limit) {
        print('Printing indexes in: ' + coll.getFullName());
        printjson(coll.getIndexes());

        print('Printing the first ' + limit + ' documents in: ' + coll.getFullName());
        var res = coll.find().limit(limit);
        while (res.hasNext()) {
            printjson(res.next());
        }
    }

    assert.eq(typeof db, 'object', 'Invalid `db` object, is the shell connected to a mongod?');
    assert.eq(typeof obj, 'object', 'The `obj` argument must be an object');
    assert(obj.hasOwnProperty('full'), 'Please specify whether to use full validation');

    var full = obj.full;

    var success = true;

    // Don't run validate on view namespaces.
    let listCollectionsRes = db.runCommand({listCollections: 1, filter: {"type": "collection"}});
    if (jsTest.options().skipValidationOnInvalidViewDefinitions && listCollectionsRes.ok === 0) {
        assert.commandFailedWithCode(listCollectionsRes, ErrorCodes.InvalidViewDefinition);
        print('Skipping validate hook because of invalid views in system.views');
        return true;
    }
    assert.commandWorked(listCollectionsRes);

    let collInfo = new DBCommandCursor(db.getMongo(), listCollectionsRes).toArray();

    for (var collDocument of collInfo) {
        var coll = db.getCollection(collDocument["name"]);
        var res = coll.validate(full);

        if (!res.ok || !res.valid) {
            print('Collection validation failed with response: ' + tojson(res));
            dumpCollection(coll, 100);
            success = false;
        }
    }
    return success;
}
