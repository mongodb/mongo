import {getRawOperationSpec} from "jstests/libs/raw_operation_utils.js";

export var CheckOrphansAreDeletedHelpers = (function() {
    function runCheck(mongosConn, shardConn, shardId) {
        // Under some conditions, migrations that hit errors simply clear the filtering metadata and
        // are expected to be lazily recovered. Recovery can happen either on step-up or upon the
        // next metadata refresh. Since this hook is run at the end of the test, there is no
        // guarantee either of these happens so we force a refresh on these collections to ensure
        // recovery happens.
        const ensureMigrationsRecovered = (shardConn, configDB) => {
            let pendingMigrations =
                configDB.getCollection("migrationCoordinators").find().toArray();
            pendingMigrations.forEach((migrationRecoveryDoc) => {
                let nss = migrationRecoveryDoc["nss"];
                assert.commandWorked(shardConn.adminCommand({_flushRoutingTableCacheUpdates: nss}));
            });
        };

        const assertCollectionEmptyWithTimeout = (database, collName) => {
            let coll = database[collName];
            let docs = [];
            assert.soon(
                () => {
                    try {
                        docs = coll.find().toArray();
                        return docs.length === 0;
                    } catch (e) {
                        jsTest.log.info('An exception occurred while checking documents in ' +
                                            coll.getFullName() +
                                            '. Will retry again unless timed out',
                                        {error: e});
                    }
                },
                () => {
                    return 'Timed out waiting for ' + coll.getFullName() + ' to be empty @ ' +
                        shardId + ', last known contents: ' + tojson(docs);
                },
                10 * 60 * 1000,
                1000);
        };

        const configDB = shardConn.getDB('config');

        ensureMigrationsRecovered(shardConn, configDB);
        assertCollectionEmptyWithTimeout(configDB, 'migrationCoordinators');

        assertCollectionEmptyWithTimeout(configDB, 'localReshardingOperations.recipient');
        assertCollectionEmptyWithTimeout(configDB, 'localReshardingOperations.donor');

        mongosConn.getDB('config').collections.find().forEach(collDoc => {
            const ns = collDoc._id;
            const tempNsArray = ns.split('.');
            const dbName = tempNsArray.shift();
            const collName = tempNsArray.join('.');

            jsTest.log.info('Checking that orphan documents on shard ' + shardId +
                            ' have been deleted from namespace ' + ns);

            let rangeDeletions = [];
            assert.soon(
                () => {
                    rangeDeletions = configDB.rangeDeletions.find({nss: ns}).toArray();
                    return rangeDeletions.length === 0;
                },
                () => {
                    try {
                        const adminDB = shardConn.getDB('admin');
                        const idleCursors =
                            adminDB
                                .aggregate([
                                    {$currentOp: {idleCursors: true, allUsers: true}},
                                    {$match: {type: 'idleCursor'}}
                                ])
                                .toArray();
                        jsTest.log.info("Idle cursors on shard " + shardId, {idleCursors});
                    } catch (e) {
                        jsTest.log.info("Failed to get idle cursors on shard " + shardId,
                                        {error: e});
                    }

                    return 'timed out waiting for rangeDeletions on ' + ns + ' to be empty @ ' +
                        shardId + ', last known contents: ' + tojson(rangeDeletions);
                });

            const coll = shardConn.getDB(dbName)[collName];

            // Find chunks that are not owned by the shard.
            const chunksQuery = (collDoc.timestamp) ? {uuid: collDoc.uuid, shard: {$ne: shardId}}
                                                    : {ns: ns, shard: {$ne: shardId}};

            const hintRes = shardConn.getDB(dbName).runCommand({
                find: collName,
                ...getRawOperationSpec(configDB),
                hint: collDoc.key,
                limit: 1,
                singleBatch: true,
            });

            if (hintRes.ok === 1) {
                // Fast path. There is an exact match between an existing index and the collection
                // shard key pattern, so we can use hint with min / max in the query.
                mongosConn.getDB('config').chunks.find(chunksQuery).forEach(chunkDoc => {
                    // Use $min/$max so this will also work with hashed and compound shard keys.
                    const orphans = coll.find({})
                                        .rawData()
                                        .collation({locale: "simple"})
                                        .hint(collDoc.key)
                                        .min(chunkDoc.min)
                                        .max(chunkDoc.max)
                                        .toArray();
                    assert.eq(0,
                              orphans.length,
                              'found orphans @ ' + shardId + ' within chunk: ' + tojson(chunkDoc) +
                                  ', orphans: ' + tojson(orphans));
                });
            } else {
                // Slow path. There is no shard key index with exactly the same pattern as the shard
                // key.

                // Get the expression that evaluates to the name of the shard key field.
                const shardKeyPattern = Object.assign({}, collDoc.key);
                const skValueExpr = Object.entries(shardKeyPattern).map(([key, value]) => {
                    if (value === "hashed") {
                        return {$toHashedIndexKey: `$${key}`};
                    }

                    return {$ifNull: [`$${key}`, null]};
                });

                // The following query is used to find notOwnedChunks on the actual shard.
                let notOwnedChunks = [];
                mongosConn.getDB('config').chunks.find(chunksQuery).forEach(chunkDoc => {
                    const min = Object.values(Object.assign({}, chunkDoc.min));
                    const max = Object.values(Object.assign({}, chunkDoc.max));
                    notOwnedChunks.push({min: min, max: max});
                });

                if (notOwnedChunks.length === 0) {
                    // There are no chunks that are not owned by the shard, so there should be no
                    // orphan documents.
                    return;
                }

                // Find documents whose shard key value falls within the boundaries of one of the
                // chunks not owned by the shard.
                const orphans = coll.aggregate([{
                    $match: {
                        $expr: {
                            $let: {
                                vars: { sk: skValueExpr },
                                in: {
                                    $anyElementTrue: [
                                        {
                                            // For each not owned chunk, verify if the current doc lies within its range.
                                            $map: {
                                                input: notOwnedChunks,
                                                as: "chunkDoc",
                                                in: {
                                                    $and: [
                                                        { $gte: ["$$sk", "$$chunkDoc.min"] },
                                                        {
                                                            $or: [
                                                                { $lt: ['$$sk', "$$chunkDoc.max"] },
                                                                {
                                                                    $allElementsTrue: [{
                                                                        $map: {
                                                                            input: "$$chunkDoc.max",
                                                                            in: { $eq: [{ $type: '$$this' }, 'maxKey'] }
                                                                        }
                                                                    }]
                                                                }
                                                            ]
                                                        }
                                                    ]
                                                }
                                            }
                                        }
                                    ]
                                }
                            }
                        }
                    }
                }], { collation: { locale: "simple" }, ...getRawOperationSpec(configDB) }).toArray();
                assert.eq(0,
                          orphans.length,
                          'found orphans @ ' + shardId + ', orphans: ' + tojson(orphans) +
                              ' for collection ' + ns);
            }
        });
    }

    return {
        runCheck: runCheck,
    };
})();
