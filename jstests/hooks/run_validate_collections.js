// Runner for validateCollections that runs full validation on all collections when loaded into
// the mongo shell.
'use strict';

(function() {
    assert.eq(typeof db, 'object', 'Invalid `db` object, is the shell connected to a mongod?');
    load('jstests/libs/parallelTester.js');

    function getDirectConnections(conn) {
        // If conn does not point to a repl set, then this function returns [conn].
        const res = conn.adminCommand({isMaster: 1});
        const connections = [];

        if (res.hasOwnProperty('hosts')) {
            for (let hostString of res.hosts) {
                connections.push(new Mongo(hostString));
            }
        } else {
            connections.push(conn);
        }

        return connections;
    }

    function getConfigConnStr() {
        const shardMap = db.adminCommand({getShardMap: 1});
        if (!shardMap.hasOwnProperty('map')) {
            throw new Error('Expected getShardMap() to return an object a "map" field: ' +
                            tojson(shardMap));
        }

        const map = shardMap.map;

        if (!map.hasOwnProperty('config')) {
            throw new Error('Expected getShardMap().map to have a "config" field: ' + tojson(map));
        }

        return map.config;
    }

    function isMongos() {
        return db.isMaster().msg === 'isdbgrid';
    }

    function getServerList() {
        const serverList = [];

        if (isMongos()) {
            // We're connected to a sharded cluster through a mongos.

            // 1) Add all the config servers to the server list.
            const configConnStr = getConfigConnStr();
            const configServerReplSetConn = new Mongo(configConnStr);
            serverList.push(...getDirectConnections(configServerReplSetConn));

            // 2) Add shard members to the server list.
            const configDB = db.getSiblingDB('config');
            const cursor = configDB.shards.find();

            while (cursor.hasNext()) {
                const shard = cursor.next();
                const shardReplSetConn = new Mongo(shard.host);
                serverList.push(...getDirectConnections(shardReplSetConn));
            }
        } else {
            // We're connected to a mongod.
            serverList.push(...getDirectConnections(db.getMongo()));
        }

        return serverList;
    }

    // Run a separate thread to validate collections on each server in parallel.
    var validateCollectionsThread = function(host, testData) {
        load('jstests/hooks/validate_collections.js');  // For validateCollections.
        TestData = testData;  // Pass the TestData object from main thread.

        try {
            print('Running validate() on ' + host);
            const conn = new Mongo(host);
            conn.setSlaveOk();
            jsTest.authenticate(conn);

            const dbNames = conn.getDBNames();
            for (let dbName of dbNames) {
                if (!validateCollections(conn.getDB(dbName), {full: true})) {
                    return {ok: 0};
                }
            }
            return {ok: 1};
        } catch (e) {
            print('Exception caught in scoped thread running validationCollections on server: ' +
                  host);
            return {ok: 0, error: e.toString(), stack: e.stack};
        }
    };

    // We run the scoped threads in a try/finally block in case any thread throws an exception, in
    // which case we want to still join all the threads.
    let threads = [];
    const serverList = getServerList();

    try {
        serverList.forEach(server => {
            const thread = new ScopedThread(validateCollectionsThread, server.host, TestData);
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
})();
