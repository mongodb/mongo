load('jstests/libs/check_orphans_are_deleted_helpers.js');  // For CheckOrphansAreDeletedHelpers.
load('jstests/sharding/autosplit_include.js');              // For waitForOngoingChunkSplits

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
                        CheckOrphansAreDeletedHelpers.runCheck(mongosConn, shardConn, shardDoc._id);
                    });
                }
            });
        });
    } else {
        assert.commandWorked(mongosConn.adminCommand({balancerStop: 1}));

        try {
            waitForOngoingChunkSplits(this);
        } catch (e) {
            print("Unable to wait for ongoing chunk splits: " + e);
        }

        // Use config.shards so we will not miss shards added outside of ShardingTest.
        mongosConn.getDB('config').shards.find().forEach(shardDoc => {
            let shardConn = getConn(shardDoc.host);

            if (shardConn != null) {
                shardConn.host = shardDoc.host;
                CheckOrphansAreDeletedHelpers.runCheck(mongosConn, shardConn, shardDoc._id);
            }
        });
    }
};
