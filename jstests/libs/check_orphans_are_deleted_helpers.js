'use strict';

load("jstests/sharding/libs/find_chunks_util.js");

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

        mongosConn.getDB('config').collections.find({dropped: false}).forEach(collDoc => {
            const ns = collDoc._id;
            const tempNsArray = ns.split('.');
            const dbName = tempNsArray.shift();
            const collName = tempNsArray.join('.');

            // It is possible for a test to drop the shard key index. We skip running the check for
            // orphan documents being deleted from that collection if it doesn't have a shard key
            // index.
            const hintRes = shardConn.getDB(dbName).runCommand({
                find: collName,
                hint: collDoc.key,
                limit: 1,
                singleBatch: true,
            });

            if (hintRes.ok !== 1) {
                assert(
                    /hint provided does not correspond to an existing index/.test(hintRes.errmsg),
                    () => {
                        return 'expected query failure due to bad hint: ' + tojson(hintRes);
                    });
                print('Failed to find shard key index on ' + ns +
                      ' so skipping check for orphan documents being deleted');
                return;
            }

            print('Checking that orphan documents on shard ' + shardId +
                  ' have been deleted from namespace ' + ns);

            let rangeDeletions = [];
            assert.soon(
                () => {
                    rangeDeletions = configDB.rangeDeletions.find({nss: ns}).toArray();
                    return rangeDeletions.length === 0;
                },
                () => {
                    return 'timed out waiting for rangeDeletions on ' + ns + ' to be empty @ ' +
                        shardId + ', last known contents: ' + tojson(rangeDeletions);
                });

            const coll = shardConn.getDB(dbName)[collName];
            findChunksUtil.findChunksByNs(mongosConn.getDB('config'), ns, {shard: {$ne: shardId}})
                .forEach(chunkDoc => {
                    // Use $min/$max so this will also work with hashed and compound shard keys.
                    const orphans = coll.find({})
                                        .hint(collDoc.key)
                                        .min(chunkDoc.min)
                                        .max(chunkDoc.max)
                                        .toArray();
                    assert.eq(0,
                              orphans.length,
                              'found orphans @ ' + shardId + ' within chunk: ' + tojson(chunkDoc) +
                                  ', orphans: ' + tojson(orphans));
                });
        });
    }

    return {
        runCheck: runCheck,
    };
})();
