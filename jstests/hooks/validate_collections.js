// Wrapper around the validate command that can be used to validate index key counts.
'use strict';

function CollectionValidator() {
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

        let success = true;

        // Don't run validate on view namespaces.
        let filter = {type: "collection"};
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
            const coll = db.getCollection(collDocument["name"]);
            const res = coll.validate(full);

            if (!res.ok || !res.valid) {
                if (jsTest.options().skipValidationOnNamespaceNotFound &&
                    res.errmsg === 'ns not found') {
                    // During a 'stopStart' backup/restore on the secondary node, the actual list of
                    // collections can be out of date if ops are still being applied from the oplog.
                    // In this case we skip the collection if the ns was not found at time of
                    // validation and continue to next.
                    print('Skipping collection validation for ' + coll.getFullName() +
                          ' since collection was not found');
                    continue;
                }
                const host = db.getMongo().host;
                print('Collection validation failed on host ' + host + ' with response: ' +
                      tojson(res));
                dumpCollection(coll, 100);
                success = false;
            }
        }

        return success;
    };

    // Run a separate thread to validate collections on each server in parallel.
    const validateCollectionsThread = function(validatorFunc, host, testData) {
        TestData = testData;  // Pass the TestData object from main thread.

        try {
            print('Running validate() on ' + host);
            const conn = new Mongo(host);
            conn.setSlaveOk();
            jsTest.authenticate(conn);

            // Skip validating collections for arbiters.
            if (conn.getDB('admin').isMaster('admin').arbiterOnly === true) {
                print('Skipping collection validation on arbiter ' + host);
                return {ok: 1};
            }

            const dbNames = conn.getDBNames();
            for (let dbName of dbNames) {
                if (!validatorFunc(conn.getDB(dbName), {full: true})) {
                    return {ok: 0, host: host};
                }
            }
            return {ok: 1};
        } catch (e) {
            print('Exception caught in scoped thread running validationCollections on server: ' +
                  host);
            return {ok: 0, error: e.toString(), stack: e.stack, host: host};
        }
    };

    this.validateNodes = function(hostList) {
        // We run the scoped threads in a try/finally block in case any thread throws an exception,
        // in which case we want to still join all the threads.
        let threads = [];

        try {
            hostList.forEach(host => {
                const thread = new ScopedThread(
                    validateCollectionsThread, this.validateCollections, host, TestData);
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
    };
}

// Ensure compatability with existing callers. Cannot use `const` or `let` here since this file may
// be loaded more than once.
var validateCollections = new CollectionValidator().validateCollections;
