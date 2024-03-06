/**
 * Returns the profiler docs for the specified database that match that specified filter.
 * Automatically retries on a CappedPositionLost error since the profiler collection is capped so
 * this error is expected.
 */
function findProfilerDocsAutoRetry(conn, dbName, filter) {
    const profilerColl = conn.getDB(dbName).system.profile;

    let profilerDocs;
    assert.soon(() => {
        try {
            profilerDocs = profilerColl.find(filter).toArray();
            return true;
        } catch (e) {
            if (e.code !== ErrorCodes.CappedPositionLost) {
                throw e;
            }
            print(`Retrying on CappedPositionLost error: ${tojson(e)}`);
            return false;
        }
    });
    return profilerDocs;
}

/**
 * Uses the profiler docs to validate that this host did not run any read commands from the shell
 * that were supposed to run on other hosts. Sets the 'numProfilerDocsPerHost' for this host to the
 * total number of profiler docs for the commands from the the shell that it finds on the host.
 */
export function validateProfilerCollections(hostDoc, hostDocs, numProfilerDocsPerHost) {
    print("Validating profiler collections on host " + tojsononeline(hostDoc));
    const conn = new Mongo(hostDoc.host);
    conn.setSecondaryOk();
    jsTest.authenticate(conn);
    numProfilerDocsPerHost[hostDoc.host] = 0

    const dbNames = conn.getDBNames();
    for (let dbName of dbNames) {
        numProfilerDocsPerHost[hostDoc.host] += findProfilerDocsAutoRetry(conn, dbName, {
                                                    ns: {$ne: dbName + ".system.profile"},
                                                    appName: "MongoDB Shell"
                                                }).length;

        const profilerDocs = findProfilerDocsAutoRetry(conn, dbName, {
            appName: "MongoDB Shell",
            // The runCommand override in set_read_preference_secondary.js attaches a "comment"
            // field to every read command.
            "command.comment": {$exists: true},
        });
        jsTest.log("Validating profiler collection for database '" + dbName + "' on host " +
                   tojsononeline({...hostDoc, numDocs: profilerDocs.length}));
        if (hostDoc.isExcluded) {
            // Verify that this host did not run any read commands from the shell since it was
            // excluded.
            assert.eq(profilerDocs.length, 0, profilerDocs);
        }

        // Verify that this host did not run any read commands from the shell that were supposed to
        // run on other hosts.
        hostDocs.forEach(otherHostDoc => {
            if (otherHostDoc.host == hostDoc.host) {
                return;
            }
            if (!otherHostDoc.hasOwnProperty("comment")) {
                assert(otherHostDoc.isPrimary || otherHostDoc.isExcluded, otherHostDoc);
                return;
            }
            const filters = [
                // The runCommand override in set_read_preference_secondary.js attaches a "comment"
                // field to every read command.
                {"command.comment": otherHostDoc.comment}
            ];
            if (hostDoc.isPrimary) {
                // Filter out writes commands since aggregate commands that involve $out and $merge
                // are expected to trigger writes on the primary.
                filters.push({
                    $or: [
                        {"command.aggregate": {$exists: true}},
                        {"command.collStats": {$exists: true}},
                        {"command.count": {$exists: true}},
                        {"command.dbStats": {$exists: true}},
                        {"command.distinct": {$exists: true}},
                        {"command.find": {$exists: true}},
                    ]
                });
                // Filter out reads against the 'config' database since read commands may trigger
                // routing metadata refresh reads on the primary.
                filters.push({ns: {$regex: /^(?!config\.).*/}});
            }
            const profilerDocs = findProfilerDocsAutoRetry(conn, dbName, {$and: filters});
            assert.eq(profilerDocs.length, 0, profilerDocs);
        });
    }
}

export function getTotalNumProfilerDocs(numProfilerDocsPerHost) {
    let totalNumProfilerDocs = 0;
    for (let host in numProfilerDocsPerHost) {
        totalNumProfilerDocs += numProfilerDocsPerHost[host];
    }
    return totalNumProfilerDocs;
}
