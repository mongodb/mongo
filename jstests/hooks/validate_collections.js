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

    function getFeatureCompatibilityVersion(adminDB) {
        var res = adminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
        if (res === null) {
            return "3.2";
        }
        return res.version;
    }

    function setFeatureCompatibilityVersion(adminDB, version) {
        var res = adminDB.runCommand({setFeatureCompatibilityVersion: version});
        if (!res.ok) {
            return res;
        }

        assert.eq(version, getFeatureCompatibilityVersion(adminDB));
        return res;
    }

    assert.eq(typeof db, 'object', 'Invalid `db` object, is the shell connected to a mongod?');
    assert.eq(typeof obj, 'object', 'The `obj` argument must be an object');
    assert(obj.hasOwnProperty('full'), 'Please specify whether to use full validation');

    var full = obj.full;

    var success = true;

    var adminDB = db.getSiblingDB("admin");

    // Set the featureCompatibilityVersion to its required value for performing validation. Save the
    // original value.
    var originalFeatureCompatibilityVersion;
    if (jsTest.options().forceValidationWithFeatureCompatibilityVersion) {
        try {
            originalFeatureCompatibilityVersion = getFeatureCompatibilityVersion(adminDB);
        } catch (e) {
            if (jsTest.options().skipValidationOnInvalidViewDefinitions &&
                e.code === ErrorCodes.InvalidViewDefinition) {
                print("Reading the featureCompatibilityVersion from the admin.system.version" +
                      " collection failed due to an invalid view definition on the admin database");
                // The view catalog would only have been resolved if the namespace doesn't exist as
                // a collection. The absence of the admin.system.version collection is equivalent to
                // having featureCompatibilityVersion=3.2.
                originalFeatureCompatibilityVersion = "3.2";
            } else {
                throw e;
            }
        }

        var res = setFeatureCompatibilityVersion(
            adminDB, jsTest.options().forceValidationWithFeatureCompatibilityVersion);
        // Bypass collections validation when setFeatureCompatibilityVersion fails with KeyTooLong
        // while forcing feature compatibility version. The KeyTooLong error response occurs as a
        // result of having a document with a large "version" field in the admin.system.version
        // collection.
        if (!res.ok && jsTest.options().forceValidationWithFeatureCompatibilityVersion === "3.4") {
            print("Skipping collection validation since forcing the featureCompatibilityVersion" +
                  " to 3.4 failed");
            assert.commandFailedWithCode(res, ErrorCodes.KeyTooLong);
            success = true;
            return success;
        } else {
            assert.commandWorked(res);
        }
    }

    // Don't run validate on view namespaces.
    let filter = {type: "collection"};
    if (jsTest.options().skipValidationOnInvalidViewDefinitions) {
        // If skipValidationOnInvalidViewDefinitions=true, then we avoid resolving the view catalog
        // on the admin database.
        //
        // TODO SERVER-25493: Remove the $exists clause once performing an initial sync from
        // versions of MongoDB <= 3.2 is no longer supported.
        filter = {$or: [filter, {type: {$exists: false}}]};
    }

    let collInfo = db.getCollectionInfos(filter);
    for (var collDocument of collInfo) {
        var coll = db.getCollection(collDocument["name"]);
        var res = coll.validate(full);

        if (!res.ok || !res.valid) {
            print('Collection validation failed with response: ' + tojson(res));
            dumpCollection(coll, 100);
            success = false;
        }
    }

    // Restore the original value for featureCompatibilityVersion.
    if (jsTest.options().forceValidationWithFeatureCompatibilityVersion) {
        assert.commandWorked(
            setFeatureCompatibilityVersion(adminDB, originalFeatureCompatibilityVersion));
    }

    return success;
}
