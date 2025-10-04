import {ReplSetTest} from "jstests/libs/replsettest.js";
import newMongoWithRetry from "jstests/libs/retryable_mongo.js";

let startTime = Date.now();
assert.neq(typeof db, "undefined", "No `db` object, is the shell connected to a mongod?");

let runCheckOnReplSet = function (db) {
    let primaryInfo = db.adminCommand("isMaster");

    assert(primaryInfo.ismaster, "shell is not connected to the primary or master node: " + tojson(primaryInfo));

    let testFixture = new ReplSetTest(db.getMongo().host);
    testFixture.checkOplogs();
};

if (db.getMongo().isMongos()) {
    let configDB = db.getSiblingDB("config");

    // Run check on every shard.
    configDB.shards.find().forEach((shardEntry) => {
        let newConn = newMongoWithRetry(shardEntry.host, undefined, {gRPC: false});
        runCheckOnReplSet(newConn.getDB("admin"));
    });

    // Run check on config server.
    let cmdLineOpts = db.adminCommand({getCmdLineOpts: 1});
    let configConn = newMongoWithRetry(cmdLineOpts.parsed.sharding.configDB, undefined, {gRPC: false});
    runCheckOnReplSet(configConn.getDB("admin"));
} else {
    runCheckOnReplSet(db);
}

let totalTime = Date.now() - startTime;
print("Finished consistency oplog checks of cluster in " + totalTime + " ms.");
