// Wrapper around the validate command that can be used to validate index key counts.
'use strict';

function CollectionValidator() {
    load('jstests/libs/feature_compatibility_version.js');
    load('jstests/libs/parallelTester.js');

    if (!(this instanceof CollectionValidator)) {
        throw new Error('Please use "new CollectionValidator()"');
    }

    this.validateCollections = function(db, obj) {
        function dumpCollection(coll, limit) {
            print('Printing indexes in: ' + coll.getFullName());
            printjson(coll.getIndexes());

            print('Printing the first ' + limit + ' documents in: ' + coll.getFullName());
            const res = coll.find().limit(limit);
            while (res.hasNext()) {
                printjson(res.next());
            }
        }

        assert.eq(typeof db, 'object', 'Invalid `db` object, is the shell connected to a mongod?');
        assert.eq(typeof obj, 'object', 'The `obj` argument must be an object');
        assert(obj.hasOwnProperty('full'), 'Please specify whether to use full validation');

        const full = obj.full;

        // Failed collection validation results are saved in failed_res.
        let full_res = {ok: 1, failed_res: []};

        // Don't run validate on view namespaces.
        let filter = {type: 'collection'};
        if (jsTest.options().skipValidationOnInvalidViewDefinitions) {
            // If skipValidationOnInvalidViewDefinitions=true, then we avoid resolving the view
            // catalog on the admin database.
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
        for (let collDocument of collInfo) {
            const coll = db.getCollection(collDocument['name']);
            const res = coll.validate(full);

            if (!res.ok || !res.valid) {
                if (jsTest.options().skipValidationOnNamespaceNotFound &&
                    res.codeName === "NamespaceNotFound") {
                    // During a 'stopStart' backup/restore on the secondary node, the actual list of
                    // collections can be out of date if ops are still being applied from the oplog.
                    // In this case we skip the collection if the ns was not found at time of
                    // validation and continue to next.
                    print('Skipping collection validation for ' + coll.getFullName() +
                          ' since collection was not found');
                    continue;
                }
                const host = db.getMongo().host;
                print('Collection validation failed on host ' + host +
                      ' with response: ' + tojson(res));
                dumpCollection(coll, 100);
                full_res.failed_res.push(res);
                full_res.ok = 0;
            }
        }

        return full_res;
    };

    // Run a separate thread to validate collections on each server in parallel.
    const validateCollectionsThread = function(validatorFunc, host) {
        try {
            load('jstests/libs/feature_compatibility_version.js');

            print('Running validate() on ' + host);
            const conn = new Mongo(host);
            conn.setSlaveOk();
            jsTest.authenticate(conn);

            const requiredFCV = jsTest.options().forceValidationWithFeatureCompatibilityVersion;
            if (requiredFCV) {
                // Make sure this node has the desired FCV as it may take time for the updates to
                // replicate to the nodes that weren't part of the w=majority.
                assert.soonNoExcept(() => {
                    checkFCV(conn.getDB('admin'), requiredFCV);
                    return true;
                });
            }

            const dbNames = conn.getDBNames();
            for (let dbName of dbNames) {
                const validateRes = validatorFunc(conn.getDB(dbName), {full: true});
                if (validateRes.ok !== 1) {
                    return {ok: 0, host: host, validateRes: validateRes};
                }
            }
            return {ok: 1};
        } catch (e) {
            print('Exception caught in scoped thread running validationCollections on server: ' +
                  host);
            return {ok: 0, error: e.toString(), stack: e.stack, host: host};
        }
    };

    this.validateNodes = function(hostList, setFCVHost) {
        // We run the scoped threads in a try/finally block in case any thread throws an exception,
        // in which case we want to still join all the threads.
        let threads = [];
        let adminDB;
        let originalFCV;

        const requiredFCV = jsTest.options().forceValidationWithFeatureCompatibilityVersion;
        if (requiredFCV) {
            let conn = new Mongo(setFCVHost);
            adminDB = conn.getDB('admin');
            originalFCV = adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});

            if (originalFCV.targetVersion) {
                // If a previous FCV upgrade or downgrade was interrupted, then we run the
                // setFeatureCompatibilityVersion command to complete it before attempting to set
                // the feature compatibility version to 'requiredFCV'.
                assert.commandWorked(adminDB.runCommand(
                    {setFeatureCompatibilityVersion: originalFCV.targetVersion}));
                checkFCV(adminDB, originalFCV.targetVersion);
            }

            if (originalFCV.version !== requiredFCV && originalFCV.targetVersion !== requiredFCV) {
                assert.commandWorked(
                    adminDB.runCommand({setFeatureCompatibilityVersion: requiredFCV}));
            }
        }

        try {
            hostList.forEach(host => {
                const thread =
                    new Thread(validateCollectionsThread, this.validateCollections, host);
                threads.push(thread);
                thread.start();
            });
        } finally {
            // Wait for each thread to finish. Throw an error if any thread fails.
            const returnData = threads.map(thread => {
                thread.join();
                return thread.returnData();
            });

            returnData.forEach(res => {
                assert.commandWorked(res, 'Collection validation failed');
            });
        }

        if (originalFCV && originalFCV.version !== requiredFCV) {
            assert.commandWorked(
                adminDB.runCommand({setFeatureCompatibilityVersion: originalFCV.version}));
        }
    };
}

// Ensure compatibility with existing callers. Cannot use `const` or `let` here since this file may
// be loaded more than once.
var validateCollections = new CollectionValidator().validateCollections;
