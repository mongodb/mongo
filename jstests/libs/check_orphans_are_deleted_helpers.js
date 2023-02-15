'use strict';

var CheckOrphansAreDeletedHelpers = (function() {
    function runCheck(mongosConn, shardConn, shardId) {
        const configDB = shardConn.getDB('config');

        let migrationCoordinatorDocs = [];
        assert.soon(
            () => {
                try {
                    migrationCoordinatorDocs = configDB.migrationCoordinators.find().toArray();
                    return migrationCoordinatorDocs.length === 0;
                } catch (exp) {
                    // Primary purpose is to stabilize shell repl set monitor to recognize the
                    // current primary.
                    print('caught exception while checking migration coordinators, ' +
                          'will retry again unless timed out: ' + tojson(exp));
                }
            },
            () => {
                return 'timed out waiting for migrationCoordinators to be empty @ ' + shardId +
                    ', last known contents: ' + tojson(migrationCoordinatorDocs);
            },
            5 * 60 * 1000,
            1000);

        mongosConn.getDB('config').collections.find().forEach(collDoc => {
            const ns = collDoc._id;
            const tempNsArray = ns.split('.');
            const dbName = tempNsArray.shift();
            const collName = tempNsArray.join('.');

            print('Checking that orphan documents on shard ' + shardId +
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
                                    {$match: {type: 'idleCursor', ns: ns}}
                                ])
                                .toArray();
                        print("Idle cursors on " + ns + " @ " + shardId + ": " +
                              tojson(idleCursors));
                    } catch (e) {
                        print("Failed to get idle cursors for " + ns + " @ " + shardId + ": " +
                              tojson(e));
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
                hint: collDoc.key,
                limit: 1,
                singleBatch: true,
            });

            if (hintRes.ok !== 1) {
                // Slow path. There is no shard key index with the same pattern as the shard key.

                // Get the expression that evaluates to the name of the shard key field.
                const shardKeyPattern = Object.assign({}, collDoc.key);
                const skValueExpr = Object.entries(shardKeyPattern).map(([key, value]) => {
                    if (value === "hashed") {
                        return {$toHashedIndexKey: `$${key}`};
                    }

                    return {$ifNull: [`$${key}`, null]};
                });

                // The following query is used to find notOwnedChunks on the actual shard.
                let arr = [];
                mongosConn.getDB('config').chunks.find(chunksQuery).forEach(chunkDoc => {
                    const min = Object.values(Object.assign({}, chunkDoc.min));
                    const max = Object.values(Object.assign({}, chunkDoc.max));
                    arr.push({min: min, max: max});
                });

                if (arr.length === 0) {
                    // There are no chunks that are not owned by the shard, so there should be no
                    // orphan documents.
                    return;
                }

                // Find documents that the shard key value is greater than or equal to the minimum
                // bound of the chunk, and less than the max bound of the chunk.
                const orphans = coll.aggregate([{
                $match: {$expr: {$let: {vars: {sk: skValueExpr},
                    in: { $allElementsTrue: [
                            { $map: {
                                input: arr,
                                as: "chunkDoc",
                                in: {
                                    $and: [
                                        {$gte: ["$$sk", "$$chunkDoc.min"]},
                                        {$or: [
                                            {$lt : ['$$sk', "$$chunkDoc.max"]},
                                            {$allElementsTrue: [{$map: {
                                                input: "$$chunkDoc.max",
                                                in: {$eq: [{$type: '$$this.max'}, 'MaxKey']}
                                            }}]}
                                        ]}
                                    ]
                                }
                            }
                            }
                        ]
                    }
                }}}}], {collation: {locale: "simple"}}).toArray();

                assert.eq(0,
                          orphans.length,
                          'found orphans @ ' + shardId + ', orphans: ' + tojson(orphans) +
                              ' for collection ' + ns);
            } else {
                // Fast path. There is a shard key index with the same pattern as the shard key, so
                // we can use hint with min / max in the query.

                mongosConn.getDB('config').chunks.find(chunksQuery).forEach(chunkDoc => {
                    // Use $min/$max so this will also work with hashed and compound shard keys.
                    const orphans = coll.find({})
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
            }
        });
    }

    return {
        runCheck: runCheck,
    };
})();
