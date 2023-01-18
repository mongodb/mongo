'use strict';

load('jstests/libs/check_shard_filtering_metadata_helpers.js');

ShardingTest.prototype.checkShardFilteringMetadata = function() {
    if (jsTest.options().skipCheckShardFilteringMetadata) {
        jsTest.log("Skipping shard filtering metadata check");
        return;
    }

    // Use a new connection so we don't have to worry about existing users logged in to the
    // connection.
    let mongosConn = new Mongo(this.s.host);
    mongosConn.fullOptions = Object.merge(this.s.fullOptions, {});

    const keyFile = this.keyFile;
    const useAuth = keyFile || mongosConn.fullOptions.clusterAuthMode === 'x509';

    let getConn = function(connStr) {
        try {
            return new Mongo(connStr);
        } catch (exp) {
            jsTest.log('CheckShardFilteringMetadata: Unable to connect to ' + connStr);
            return null;
        }
    };

    function executeAuthenticatedIfNeeded(conn, fn) {
        if (useAuth) {
            return authutil.asCluster(conn, keyFile, fn);
        } else {
            return fn();
        }
    }

    executeAuthenticatedIfNeeded(mongosConn, () => {
        // For each shard
        mongosConn.getDB('config').shards.find().forEach(shardDoc => {
            const shardName = shardDoc._id;
            const shardConn = getConn(shardDoc.host);
            if (shardConn === null) {
                jsTest.log('CheckShardFilteringMetadata: Skipping check on shard' + shardDoc.host);
                return;
            }
            shardConn.fullOptions = Object.merge(this.s.fullOptions, {});

            // Await replication to ensure that metadata on secondary nodes is up-to-date.
            this.awaitReplicationOnShards();

            // Get nodes for this shard
            shardConn.setSecondaryOk();
            const shardNodesHosts = executeAuthenticatedIfNeeded(shardConn, () => {
                return shardConn.adminCommand({replSetGetConfig: 1})
                    .config.members.filter(member => member.arbiterOnly === false)
                    .map(member => member.host);
            });

            // Run check on each node
            shardNodesHosts.forEach(host => {
                const shardNodeConn = getConn(host);
                if (shardNodeConn === null) {
                    jsTest.log('CheckShardFilteringMetadata: Skipping check on node' +
                               shardDoc.host);
                    return;
                }
                shardNodeConn.fullOptions = Object.merge(this.s.fullOptions, {});

                executeAuthenticatedIfNeeded(shardNodeConn, () => {
                    CheckShardFilteringMetadataHelpers.run(mongosConn, shardNodeConn, shardName);
                });
            });
        });
    });
};
