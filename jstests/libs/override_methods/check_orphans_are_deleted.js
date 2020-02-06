/**
 * Asserts that all shards in the sharded cluster doesn't own any orphan documents.
 * Requires all shards and config server to have primary that is reachable.
 *
 * Note: Doesn't catch documents in the shard that doesn't have the full shard key.
 * Assumes that all shards have the index that matches the shard key.
 */
ShardingTest.prototype.checkOrphansAreDeleted = function() {
    if (jsTest.options().skipCheckOrphans) {
        print("Skipping orphan check across the cluster");
        return;
    }

    print('Running check orphans against cluster with mongos: ' + this.s.host);

    let runCheck = function(mongosConn, shardConn, shardId) {
        let configDB = shardConn.getDB('config');

        let migrationCoordinatorDocs = [];
        assert.soon(
            () => {
                try {
                    migrationCoordinatorDocs = configDB.migrationCoordinators.find().toArray();
                    return migrationCoordinatorDocs.length == 0;
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

        let rangeDeletions = [];
        assert.soon(
            () => {
                rangeDeletions = configDB.rangeDeletions.find().toArray();
                return rangeDeletions.length == 0;
            },
            () => {
                return 'timed out waiting for rangeDeletions to be empty @ ' + shardId +
                    ', last known contents: ' + tojson(rangeDeletions);
            });

        mongosConn.getDB('config').collections.find({dropped: false}).forEach(collDoc => {
            let tempNsArray = collDoc._id.split('.');
            let dbName = tempNsArray.shift();
            let collName = tempNsArray.join('.');

            let coll = shardConn.getDB(dbName)[collName];
            mongosConn.getDB('config')
                .chunks.find({ns: collDoc._id, shard: {$ne: shardId}})
                .forEach(chunkDoc => {
                    // Use $min/$max so this will also work with hashed and compound shard keys.
                    let orphans = coll.find({})
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
    };

    let getConn = function(connStr) {
        try {
            return new Mongo(connStr);
        } catch (exp) {
            jsTest.log('Unable to connect to ' + connStr + ' while trying to check for orphans');
            return null;
        }
    };

    // Use a new connection so we don't have to worry about existing users logged in to the
    // connection.
    let mongosConn = new Mongo(this.s.host);
    mongosConn.fullOptions = Object.merge(this.s.fullOptions, {});

    const keyFile = this.keyFile;
    if (keyFile || mongosConn.fullOptions.clusterAuthMode == 'x509') {
        authutil.asCluster(mongosConn, keyFile, () => {
            assert.commandWorked(mongosConn.adminCommand({balancerStop: 1}));

            // Use config.shards so we will not miss shards added outside of ShardingTest.
            mongosConn.getDB('config').shards.find().forEach(shardDoc => {
                let shardConn = getConn(shardDoc.host);

                // Inherit connection options from mongos connection.
                shardConn.fullOptions = Object.merge(this.s.fullOptions, {});

                if (shardConn != null) {
                    authutil.asCluster(shardConn, keyFile, () => {
                        runCheck(mongosConn, shardConn, shardDoc._id);
                    });
                }
            });
        });
    } else {
        assert.commandWorked(mongosConn.adminCommand({balancerStop: 1}));

        // Use config.shards so we will not miss shards added outside of ShardingTest.
        mongosConn.getDB('config').shards.find().forEach(shardDoc => {
            let shardConn = getConn(shardDoc.host);

            if (shardConn != null) {
                shardConn.host = shardDoc.host;
                runCheck(mongosConn, shardConn, shardDoc._id);
            }
        });
    }
};
