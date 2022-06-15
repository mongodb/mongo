/**
 * Starts up a cluster with all default configurations required by a serverless test.
 * The cluster has a mongoq, a config server with 3 nodes and 2 shards. Each shard has 3 nodes.
 * The X509 authentication is disabled in the cluster.
 */
class ServerlessTest {
    constructor() {
        let numShards = 2;

        this.stop = () => {
            jsTest.log("Going to stop mongoq.");
            MongoRunner.stopMongoq(this.q);

            jsTest.log("Going to stop all replica sets.");
            for (var i = 0; i < numShards; i++) {
                let rs = this["rs" + i];
                rs.stopSet(15);
            }

            jsTest.log("Going to stop config server.");
            this.configRS.stopSet();
        };

        jsTest.log("Going to create and start config server.");
        this.configRS = new ReplSetTest({name: "configRS", nodes: 3, useHostName: true});
        this.configRS.startSet({configsvr: '', storageEngine: 'wiredTiger'});

        jsTest.log("Initiate config server before starting mongoq.");
        let replConfig = this.configRS.getReplSetConfig();
        replConfig.configsvr = true;
        this.configRS.initiate(replConfig);

        jsTest.log("Going to start mongoq.");
        this.q = MongoRunner.runMongoq({configdb: this.configRS.getURL()});
        assert.neq(this.q, null, "Failed to start mongoq");

        jsTest.log("Going to add replica sets.");
        let adminDB = this.q.getDB('admin');
        for (let i = 0; i < numShards; i++) {
            let rs =
                new ReplSetTest({name: "testShard-rs-" + i, nodes: 3, nodeOptions: {shardsvr: ""}});
            rs.startSet({setParameter: {tenantMigrationDisableX509Auth: true}});
            rs.initiate();
            this["rs" + i] = rs;
        }

        jsTest.log("Going to create connection with each shard.");
        for (let i = 0; i < numShards; i++) {
            let rs = this["rs" + i];
            var result = assert.commandWorked(adminDB.runCommand({addShard: rs.getURL()}));

            let rsConn = new Mongo(rs.getURL());
            rsConn.name = rs.getURL();
            rsConn.rs = rs;
            rsConn.shardName = result.shardAdded;
            this["shard" + i] = rsConn;
        }

        this.q0 = this.q;
        jsTest.log("ServerlessTest is created.");
    }

    /**
     * Helper method for setting primary shard of a database and making sure that it was
     * successful. Note: first mongoq needs to be up.
     */
    ensurePrimaryShard(dbName, shardName) {
        var db = this.q.getDB('admin');
        var res = db.adminCommand({movePrimary: dbName, to: shardName});
        assert(res.ok || res.errmsg == "it is already the primary", tojson(res));
    }

    addTenant(tenantId, shardId) {
        return assert.commandWorked(
            this.configRS.getPrimary()
                .getCollection('config.tenants')
                .insert({_id: tenantId, shardId: shardId}, {writeConcern: {w: "majority"}}));
    }

    removeTenant(tenantId) {
        return assert.commandWorked(
            this.configRS.getPrimary().getCollection('config.tenants').remove({_id: tenantId}, {
                writeConcern: {w: "majority"}
            }));
    }
}
