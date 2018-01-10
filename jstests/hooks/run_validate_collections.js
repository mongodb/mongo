// Runner for validateCollections that runs full validation on all collections when loaded into
// the mongo shell.
'use strict';

(function() {
    assert.eq(typeof db, 'object', 'Invalid `db` object, is the shell connected to a mongod?');

    function getConnectionStrings(conn) {
        // If conn does not point to a repl set, then this function returns [conn].
        const res = conn.adminCommand({isMaster: 1});
        let hostList = [];

        if (res.hasOwnProperty('setName')) {
            hostList = res.hosts;
            if (res.hasOwnProperty('passives')) {
                hostList = hostList.concat(res.passives);
            }
            return hostList;
        } else {
            return [conn.host];
        }
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

    function getHostList() {
        let hostList = [];

        if (isMongos()) {
            // We're connected to a sharded cluster through a mongos.

            // 1) Add all the config servers to the server list.
            const configConnStr = getConfigConnStr();
            const configServerReplSetConn = new Mongo(configConnStr);
            hostList = getConnectionStrings(configServerReplSetConn);

            // 2) Add shard members to the server list.
            const configDB = db.getSiblingDB('config');
            const cursor = configDB.shards.find();

            while (cursor.hasNext()) {
                const shard = cursor.next();
                const shardReplSetConn = new Mongo(shard.host);
                hostList.push(...getConnectionStrings(shardReplSetConn));
            }
        } else {
            // We're connected to a mongod.
            hostList = getConnectionStrings(db.getMongo());
        }

        return hostList;
    }

    load('jstests/hooks/validate_collections.js');  // For CollectionValidator.
    new CollectionValidator().validateNodes(getHostList());

})();
