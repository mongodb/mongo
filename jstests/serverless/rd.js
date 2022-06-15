/**
 * Set up a mocked Rd which supports to add and remove entries from config.shards.
 */

class Rd {
    constructor() {
        jsTest.log("Going to create and start Rd.");
        this.rs = new ReplSetTest({name: "Rd", nodes: 3, useHostName: true});
        this.rs.startSet({storageEngine: 'wiredTiger'});
        this.rs.initiate();

        jsTest.log("Going to create connection with Rd.");
        this.conn = new Mongo(this.rs.getURL());
    }

    addShard(shardId, shardUrl) {
        jsTestLog("Add entry to config.shards: " + shardId);

        let coll = this.conn.getCollection("config.shards");
        let request = {insert: coll.getName(), documents: [{_id: shardId, host: shardUrl}]};
        assert.commandWorked(coll.runCommand(request));
    }

    removeShard(shardId) {
        jsTestLog("Remove entry from config.shards: " + shardId);

        let coll = this.conn.getCollection("config.shards");
        let request = {delete: coll.getName(), deletes: [{q: {_id: shardId}, limit: 1}]};
        assert.commandWorked(coll.runCommand(request));
    }

    stop() {
        this.rs.stopSet();
    }
}
