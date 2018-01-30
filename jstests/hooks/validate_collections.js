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
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: version}));
        assert.eq(version, getFeatureCompatibilityVersion(adminDB));
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

        setFeatureCompatibilityVersion(
            adminDB, jsTest.options().forceValidationWithFeatureCompatibilityVersion);
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

    // Optionally skip collections.
    if (Array.isArray(jsTest.options().skipValidationNamespaces) &&
        jsTest.options().skipValidationNamespaces.length > 0) {
        let skippedCollections = [];
        for (let ns of jsTest.options().skipValidationNamespaces) {
            // Strip off the database name from 'ns' to extract the collName.
            const collName = ns.replace(new RegExp('^' + db.getName() + '\.'), '');
            // Skip the collection 'collName' if the db name was removed from 'ns'.
            if (collName !== ns) {
                skippedCollections.push({name: {$ne: collName}});
            }
        }
        filter = {$and: [filter, ...skippedCollections]};
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
        setFeatureCompatibilityVersion(adminDB, originalFeatureCompatibilityVersion);
    }

    return success;
}
