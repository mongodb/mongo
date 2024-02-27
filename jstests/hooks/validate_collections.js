// Wrapper around the validate command that can be used to validate index key counts.
import {Thread} from "jstests/libs/parallelTester.js";

export class CollectionValidator {
    validateCollections(db, obj) {
        return validateCollectionsImpl(db, obj);
    }

    validateNodes(hostList) {
        // We run the scoped threads in a try/finally block in case any thread throws an exception,
        // in which case we want to still join all the threads.
        let threads = [];

        try {
            hostList.forEach(host => {
                const thread = new Thread(validateCollectionsThread, validateCollectionsImpl, host);
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
    }
}

function validateCollectionsImpl(db, obj) {
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
            // Attempt to strip the name of the database we are about to validate off of the
            // namespace we wish to skip. If the replace() function does find a match with the
            // database, then we know that the collection we want to skip is in the database we
            // are about to validate. We will then put it in the 'filter' for later use.
            const collName = ns.replace(new RegExp('^' + db.getName() + '\.'), '');
            if (collName !== ns) {
                skippedCollections.push({name: {$ne: collName}});
            }
        }
        filter = {$and: [filter, ...skippedCollections]};
    }

    // In a sharded cluster with in-progress validate command for the config database
    // (i.e. on the config server), a listCommand command on a mongos or shardsvr mongod that
    // has stale routing info may fail since a refresh would involve running read commands
    // against the config database. The read commands are lock free so they are not blocked by
    // the validate command and instead are subject to failing with a ObjectIsBusy error. Since
    // this is a transient state, we shoud retry.
    let collInfo;
    assert.soonNoExcept(() => {
        collInfo = db.getCollectionInfos(filter);
        return true;
    });

    for (let collDocument of collInfo) {
        const coll = db.getCollection(collDocument['name']);
        const res = coll.validate(obj);

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
            } else if (res.codeName === "CommandNotSupportedOnView") {
                // Even though we pass a filter to getCollectionInfos() to only fetch
                // collections, nothing is preventing the collection from being dropped and
                // recreated as a view.
                print('Skipping collection validation for ' + coll.getFullName() +
                      ' as it is a view');
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
}

// Run a separate thread to validate collections on each server in parallel.
function validateCollectionsThread(validatorFunc, host) {
    try {
        print('Running validate() on ' + host);
        const conn = new Mongo(host);
        conn.setSecondaryOk();
        jsTest.authenticate(conn);

        // Skip validating collections for arbiters.
        if (conn.getDB('admin').isMaster('admin').arbiterOnly === true) {
            print('Skipping collection validation on arbiter ' + host);
            return {ok: 1};
        }

        let requiredFCV = jsTest.options().forceValidationWithFeatureCompatibilityVersion;
        if (requiredFCV) {
            requiredFCV = new Function(
                `return typeof ${requiredFCV} === "string" ? ${requiredFCV} : "${requiredFCV}"`)();
            // Make sure this node has the desired FCV as it may take time for the updates to
            // replicate to the nodes that weren't part of the w=majority.
            assert.soonNoExcept(() => {
                checkFCV(conn.getDB('admin'), requiredFCV);
                return true;
            });
        }

        const dbNames = conn.getDBNames();
        for (let dbName of dbNames) {
            const validateRes = validatorFunc(conn.getDB(dbName), {
                full: true,
                // TODO (SERVER-24266): Always enforce fast counts, once they are always
                // accurate.
                enforceFastCount:
                    !TestData.skipEnforceFastCountOnValidate && !TestData.allowUncleanShutdowns,
                enforceTimeseriesBucketsAreAlwaysCompressed:
                    !TestData.skipEnforceTimeseriesBucketsAreAlwaysCompressedOnValidate,
                warnOnSchemaValidation: true
            });
            if (validateRes.ok !== 1) {
                return {ok: 0, host: host, validateRes: validateRes};
            }
        }
        return {ok: 1};
    } catch (e) {
        print('Exception caught in scoped thread running validationCollections on server: ' + host);
        return {ok: 0, error: e.toString(), stack: e.stack, host: host};
    }
}

// Ensure compatibility with existing callers. Cannot use `const` or `let` here since this file may
// be loaded more than once.
export const validateCollections = new CollectionValidator().validateCollections;
