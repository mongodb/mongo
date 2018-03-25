// Runner for validateCollections that runs full validation on all collections when loaded into
// the mongo shell.
'use strict';

(function() {
    assert.eq(typeof db, 'object', 'Invalid `db` object, is the shell connected to a mongod?');
    load('jstests/hooks/validate_collections.js');  // For validateCollections.

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

    const serverList = getServerList();
    for (let server of serverList) {
        print('Running validate() on ' + server.host);
        server.setSlaveOk();
        jsTest.authenticate(server);

        const dbNames = server.getDBNames();
        for (let dbName of dbNames) {
            if (!validateCollections(server.getDB(dbName), {full: true})) {
                throw new Error('Collection validation failed');
            }
        }
    }
})();
