/**
 * Basic testing for minOpTimeRecovery document. Tests that it will be created after a migration
 * only if the config server is a replica set and recovery will not run when disabled.
 *
 * This test restarts a shard and the shard will attempt to read a document that was saved before
 * the restart, so it cannot be run on ephemeral storage engines.
 * @tags: [requires_persistence]
 */
(function() {
    "use strict";

    var runTest = function(withRecovery) {
        var st = new ShardingTest({shards: 2});

        var testDB = st.s.getDB('test');
        testDB.adminCommand({enableSharding: 'test'});
        st.ensurePrimaryShard('test', 'shard0000');
        testDB.adminCommand({shardCollection: 'test.user', key: {x: 1}});

        var opTimeBeforeMigrate = null;
        if (st.configRS) {
            var priConn = st.configRS.getPrimary();
            var replStatus = priConn.getDB('admin').runCommand({replSetGetStatus: 1});
            replStatus.members.forEach(function(memberState) {
                if (memberState.state == 1) {  // if primary
                    opTimeBeforeMigrate = memberState.optime;

                    assert.neq(null, opTimeBeforeMigrate);
                    assert.neq(null, opTimeBeforeMigrate.ts);
                    assert.neq(null, opTimeBeforeMigrate.t);
                }
            });
        }

        testDB.adminCommand({moveChunk: 'test.user', find: {x: 0}, to: 'shard0001'});

        var shardAdmin = st.d0.getDB('admin');
        var doc = shardAdmin.system.version.findOne();

        if (st.configRS) {
            assert.neq(null, doc);
            assert.eq('minOpTimeRecovery', doc._id);
            assert.eq(st.configRS.getURL(), doc.configsvrConnectionString);
            assert.eq('shard0000', doc.shardName);
            assert.gt(doc.minOpTime.ts.getTime(), 0);
        } else {
            assert.eq(null, doc);
        }

        var restartCmdLineOptions = Object.merge(
            st.d0.fullOptions,
            {
              setParameter: 'recoverShardingState=' + (withRecovery ? 'true' : 'false'),
              restart: true
            });

        // Restart the shard that donated a chunk to trigger the optime recovery logic.
        st.stopMongod(0);
        var newMongod = MongoRunner.runMongod(restartCmdLineOptions);
        var shardingSection = newMongod.getDB('admin').runCommand({serverStatus: 1}).sharding;

        if (st.configRS && withRecovery) {
            assert.neq(null, shardingSection);

            // Confirm that the config server string points to an actual config server replica set.
            var configConnStr = shardingSection.configsvrConnectionString;
            var configConn = new Mongo(configConnStr);
            var configIsMaster = configConn.getDB('admin').runCommand({isMaster: 1});
            assert.gt(configConnStr.indexOf('/'), 0);
            assert.eq(1, configIsMaster.configsvr);  // If it's a shard, this field won't exist.

            var configOpTimeObj = shardingSection.lastSeenConfigServerOpTime;
            assert.neq(null, configOpTimeObj);
            assert.gte(configOpTimeObj.ts.getTime(), opTimeBeforeMigrate.ts.getTime());
            assert.gte(configOpTimeObj.t, opTimeBeforeMigrate.t);
        } else {
            assert.eq(null, shardingSection);
        }

        MongoRunner.stopMongod(newMongod.port);
        st.stop();
    };

    runTest(true);
    runTest(false);

})();
